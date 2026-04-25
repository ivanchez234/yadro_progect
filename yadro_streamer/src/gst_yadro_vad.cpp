#include "gst_yadro_vad.h"

GST_DEBUG_CATEGORY_STATIC (gst_yadro_vad_debug);
#define GST_CAT_DEFAULT gst_yadro_vad_debug

#define VAD_CAPS "audio/x-raw, format=S16LE, rate=16000, channels=1"
#define CHUNK_SIZE_BYTES 960

/* Значения по умолчанию */
#define DEFAULT_VAD_MODE 3
#define DEFAULT_HANGOVER_MS 210

/* Перечисление ID наших свойств */
enum {
    PROP_0,
    PROP_VAD_MODE,
    PROP_HANGOVER_TIME
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(VAD_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(VAD_CAPS));

G_DEFINE_TYPE (GstYadroVad, gst_yadro_vad, GST_TYPE_BASE_TRANSFORM);

// === НОВОЕ: Объявляем функцию-перехватчик событий ===
static gboolean gst_yadro_vad_sink_event(GstBaseTransform *trans, GstEvent *event);

/* --- ОБРАБОТЧИКИ СВОЙСТВ --- */
static void gst_yadro_vad_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    GstYadroVad *filter = GST_YADRO_VAD(object);

    switch (prop_id) {
        case PROP_VAD_MODE:
            filter->vad_mode = g_value_get_int(value);
            if (filter->vad_inst) {
                fvad_set_mode(filter->vad_inst, filter->vad_mode);
            }
            break;
        case PROP_HANGOVER_TIME:
            filter->hangover_duration_ms = g_value_get_int(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void gst_yadro_vad_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    GstYadroVad *filter = GST_YADRO_VAD(object);

    switch (prop_id) {
        case PROP_VAD_MODE:
            g_value_set_int(value, filter->vad_mode);
            break;
        case PROP_HANGOVER_TIME:
            g_value_set_int(value, filter->hangover_duration_ms);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/* --- ОСНОВНАЯ ЛОГИКА --- */
static gboolean gst_yadro_vad_start(GstBaseTransform *trans) {
    GstYadroVad *filter = GST_YADRO_VAD(trans);
    filter->adapter = gst_adapter_new();
    filter->vad_inst = fvad_new();
    
    fvad_set_mode(filter->vad_inst, filter->vad_mode);
    fvad_set_sample_rate(filter->vad_inst, 16000);
    
    filter->state = VAD_STATE_SILENCE;
    filter->hangover_time_left_ms = 0;
    filter->original_time = 0;
    filter->total_dropped_time = 0;
    filter->need_discont = FALSE;
    
    GST_INFO_OBJECT(filter, "Started. VAD Mode: %d, Hangover: %d ms", filter->vad_mode, filter->hangover_duration_ms);
    return TRUE;
}

static gboolean gst_yadro_vad_stop(GstBaseTransform *trans) {
    GstYadroVad *filter = GST_YADRO_VAD(trans);
    if (filter->adapter) g_object_unref(filter->adapter);
    if (filter->vad_inst) fvad_free(filter->vad_inst);
    return TRUE;
}

static GstFlowReturn gst_yadro_vad_submit_input_buffer(GstBaseTransform *trans, gboolean is_discont, GstBuffer *input) {
    GstYadroVad *filter = GST_YADRO_VAD(trans);
    gst_adapter_push(filter->adapter, input);
    return GST_FLOW_OK;
}

static GstFlowReturn gst_yadro_vad_generate_output(GstBaseTransform *trans, GstBuffer **outbuf) {
    GstYadroVad *filter = GST_YADRO_VAD(trans);
    gsize available = gst_adapter_available(filter->adapter);

    if (available < CHUNK_SIZE_BYTES) {
        *outbuf = NULL; 
        return GST_FLOW_OK;
    }

    GstBuffer *temp_buf = gst_adapter_take_buffer(filter->adapter, CHUNK_SIZE_BYTES);
    GstMapInfo map;
    gst_buffer_map(temp_buf, &map, GST_MAP_READ);

    int is_speech = fvad_process(filter->vad_inst, (const int16_t *)map.data, map.size / 2);
    gst_buffer_unmap(temp_buf, &map);
    GstYadroVadState old_state = filter->state;

    // === НОВОЕ: Увеличиваем общий счетчик кадров ===
    filter->total_frames++;

    if (is_speech == 1) {
        filter->state = VAD_STATE_SPEECH;
        filter->hangover_time_left_ms = filter->hangover_duration_ms;
    } else {
        if (filter->state == VAD_STATE_SPEECH || filter->state == VAD_STATE_HANGOVER) {
            filter->state = VAD_STATE_HANGOVER;
            filter->hangover_time_left_ms -= 30;
            if (filter->hangover_time_left_ms <= 0) {
                filter->state = VAD_STATE_SILENCE;
            }
        }
    }

    if (old_state == VAD_STATE_SILENCE && filter->state == VAD_STATE_SPEECH) {
        filter->need_discont = TRUE;
    }

    if (filter->state == VAD_STATE_SILENCE) {
        // === НОВОЕ: Считаем вырезанные кадры ===
        filter->frames_dropped++;
        
        filter->total_dropped_time += 30 * GST_MSECOND;
        filter->original_time += 30 * GST_MSECOND;
        gst_buffer_unref(temp_buf);
        *outbuf = NULL;
    } else {
        // === НОВОЕ: Считаем сохраненные кадры ===
        filter->frames_kept++;

        GST_BUFFER_PTS(temp_buf) = filter->original_time - filter->total_dropped_time;
        GST_BUFFER_DURATION(temp_buf) = 30 * GST_MSECOND;
        if (filter->need_discont) {
            GST_BUFFER_FLAG_SET(temp_buf, GST_BUFFER_FLAG_DISCONT);
            filter->need_discont = FALSE;
        }
        filter->original_time += 30 * GST_MSECOND;
        *outbuf = temp_buf;
    }

    return GST_FLOW_OK;
}

// === НОВОЕ: Функция-перехватчик для отправки телеметрии ===
static gboolean gst_yadro_vad_sink_event(GstBaseTransform *trans, GstEvent *event) {
    GstYadroVad *filter = GST_YADRO_VAD(trans);

    // Если аудиофайл закончился (End Of Stream)
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
        
        // Создаем посылку с нашими данными
        GstStructure *stats = gst_structure_new("YadroVadStats",
            "total_frames", G_TYPE_UINT64, filter->total_frames,
            "frames_kept",  G_TYPE_UINT64, filter->frames_kept,
            "frames_dropped", G_TYPE_UINT64, filter->frames_dropped,
            NULL);

        // Отправляем сообщение в шину GStreamer
        GstMessage *msg = gst_message_new_element(GST_OBJECT(filter), stats);
        gst_element_post_message(GST_ELEMENT(filter), msg);
        
        GST_INFO_OBJECT(filter, "Telemetry sent! Total: %lu, Kept: %lu, Dropped: %lu", 
                        filter->total_frames, filter->frames_kept, filter->frames_dropped);
    }

    // Передаем событие дальше по цепочке
    return GST_BASE_TRANSFORM_CLASS(gst_yadro_vad_parent_class)->sink_event(trans, event);
}


/* --- ИНИЦИАЛИЗАЦИЯ КЛАССА И СВОЙСТВ --- */
static void gst_yadro_vad_class_init(GstYadroVadClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_yadro_vad_set_property;
    gobject_class->get_property = gst_yadro_vad_get_property;

    g_object_class_install_property(gobject_class, PROP_VAD_MODE,
        g_param_spec_int("vad-mode", "VAD Mode",
            "Aggressiveness mode of the VAD (0=Normal, 1=Low Bitrate, 2=Aggressive, 3=Very Aggressive)",
            0, 3, DEFAULT_VAD_MODE,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_HANGOVER_TIME,
        g_param_spec_int("hangover-time", "Hangover Time (ms)",
            "Time in milliseconds to keep speech state after silence is detected",
            0, 2000, DEFAULT_HANGOVER_MS,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_add_static_pad_template(element_class, &src_template);
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_set_static_metadata(element_class,
        "YADRO VAD Filter", "Filter/Effect/Audio",
        "Removes silence and sends telemetry", "Developer");

    trans_class->start = GST_DEBUG_FUNCPTR(gst_yadro_vad_start);
    trans_class->stop = GST_DEBUG_FUNCPTR(gst_yadro_vad_stop);
    trans_class->submit_input_buffer = GST_DEBUG_FUNCPTR(gst_yadro_vad_submit_input_buffer);
    trans_class->generate_output = GST_DEBUG_FUNCPTR(gst_yadro_vad_generate_output);
    
    // === НОВОЕ: Привязываем наш перехватчик событий к классу ===
    trans_class->sink_event = GST_DEBUG_FUNCPTR(gst_yadro_vad_sink_event);
}

static void gst_yadro_vad_init(GstYadroVad *filter) {
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(filter), FALSE);
    
    filter->vad_mode = DEFAULT_VAD_MODE;
    filter->hangover_duration_ms = DEFAULT_HANGOVER_MS;

    filter->total_frames = 0;
    filter->frames_kept = 0;
    filter->frames_dropped = 0;
}

static gboolean plugin_init(GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT(gst_yadro_vad_debug, "yadrovad", 0, "Yadro VAD plugin");
    return gst_element_register(plugin, "yadrovad", GST_RANK_NONE, GST_TYPE_YADRO_VAD);
}

#define PACKAGE "yadrovad"
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR, yadrovad, "YADRO Voice Activity Detection",
    plugin_init, "1.0", "LGPL", "YadroStreamer", "http://yadro.com"
)