/*
 * yadrovad — элемент GStreamer, который вырезает паузы из речи на лету.
 *
 * Вход режется на кадры по 30 мс, каждый кадр классифицируется libfvad
 * (WebRTC VAD). Кадры речи и кадры «удержания» (hangover) после речи
 * проходят дальше, тишина выбрасывается. Метки времени пересчитываются
 * встык, поэтому на выходе непрерывный поток без пауз; на каждой склейке
 * ставится флаг DISCONT.
 */
#include "gst_yadro_vad.h"

GST_DEBUG_CATEGORY_STATIC(gst_yadro_vad_debug);
#define GST_CAT_DEFAULT gst_yadro_vad_debug

#define VAD_CAPS "audio/x-raw, format=(string)S16LE, rate=(int)16000, channels=(int)1, layout=(string)interleaved"
#define FRAME_DURATION (YADRO_VAD_FRAME_MS * GST_MSECOND)

#define DEFAULT_VAD_MODE    3
#define DEFAULT_HANGOVER_MS 210

enum {
    PROP_0,
    PROP_VAD_MODE,
    PROP_HANGOVER_TIME
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(VAD_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(VAD_CAPS));

G_DEFINE_TYPE(GstYadroVad, gst_yadro_vad, GST_TYPE_BASE_TRANSFORM);

/* Сколько кадров держать после речи. Округляем вверх: 210 мс → 7 кадров,
 * чтобы удержание было не короче заявленного. */
static guint hangover_frames_for(gint hangover_ms) {
    return (guint)((hangover_ms + YADRO_VAD_FRAME_MS - 1) / YADRO_VAD_FRAME_MS);
}

/* Сброс состояния разбора потока (старт, flush, конец потока). */
static void reset_stream_state(GstYadroVad *self) {
    if (self->adapter)
        gst_adapter_clear(self->adapter);
    self->state = VAD_STATE_SILENCE;
    self->hangover_frames_left = 0;
    self->out_time = 0;
    self->dropped_since_last = FALSE;
}

static void reset_stats(GstYadroVad *self) {
    self->total_frames = 0;
    self->frames_kept = 0;
    self->frames_dropped = 0;
    if (self->mask)
        g_string_truncate(self->mask, 0);
}

/* ---------------------------------------------------------------- свойства */

static void gst_yadro_vad_set_property(GObject *object, guint prop_id,
                                       const GValue *value, GParamSpec *pspec) {
    GstYadroVad *self = GST_YADRO_VAD(object);

    /* Свойства меняются из потока GUI во время воспроизведения. Сам fvad
     * здесь не трогаем — его использует потоковый поток. Новый режим
     * подхватится в generate_output перед следующим кадром. */
    GST_OBJECT_LOCK(self);
    switch (prop_id) {
    case PROP_VAD_MODE:
        self->vad_mode = g_value_get_int(value);
        break;
    case PROP_HANGOVER_TIME:
        self->hangover_ms = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
    GST_OBJECT_UNLOCK(self);
}

static void gst_yadro_vad_get_property(GObject *object, guint prop_id,
                                       GValue *value, GParamSpec *pspec) {
    GstYadroVad *self = GST_YADRO_VAD(object);

    GST_OBJECT_LOCK(self);
    switch (prop_id) {
    case PROP_VAD_MODE:
        g_value_set_int(value, self->vad_mode);
        break;
    case PROP_HANGOVER_TIME:
        g_value_set_int(value, self->hangover_ms);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
    GST_OBJECT_UNLOCK(self);
}

/* ------------------------------------------------------------ жизненный цикл */

static gboolean gst_yadro_vad_start(GstBaseTransform *trans) {
    GstYadroVad *self = GST_YADRO_VAD(trans);

    self->vad = fvad_new();
    if (!self->vad) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("fvad_new() failed"), (NULL));
        return FALSE;
    }
    fvad_set_sample_rate(self->vad, YADRO_VAD_RATE);

    GST_OBJECT_LOCK(self);
    self->applied_vad_mode = self->vad_mode;
    gint hangover_ms = self->hangover_ms;
    GST_OBJECT_UNLOCK(self);
    fvad_set_mode(self->vad, self->applied_vad_mode);

    self->adapter = gst_adapter_new();
    self->mask = g_string_new(NULL);
    reset_stream_state(self);
    reset_stats(self);

    GST_INFO_OBJECT(self, "started: vad-mode=%d hangover=%d ms (%u frames)",
                    self->applied_vad_mode, hangover_ms, hangover_frames_for(hangover_ms));
    return TRUE;
}

static gboolean gst_yadro_vad_stop(GstBaseTransform *trans) {
    GstYadroVad *self = GST_YADRO_VAD(trans);

    g_clear_object(&self->adapter);
    if (self->vad) {
        fvad_free(self->vad);
        self->vad = NULL;
    }
    if (self->mask) {
        g_string_free(self->mask, TRUE);
        self->mask = NULL;
    }
    return TRUE;
}

/* --------------------------------------------------------------- обработка */

static GstFlowReturn gst_yadro_vad_submit_input_buffer(GstBaseTransform *trans,
                                                       gboolean is_discont,
                                                       GstBuffer *input) {
    GstYadroVad *self = GST_YADRO_VAD(trans);

    /* Смотрим на флаг самого буфера, а не на is_discont: базовый класс
     * держит is_discont = TRUE, пока не уйдёт первый выходной буфер, а мы
     * на тишине ничего не выдаём — хвост кадра терялся бы зря. */
    if (GST_BUFFER_IS_DISCONT(input) && gst_adapter_available(self->adapter) > 0) {
        /* Разрыв во входе: недобранный хвост кадра склеивать с новыми
         * данными нельзя — получился бы кадр из двух кусков. */
        GST_DEBUG_OBJECT(self, "input discont, dropping %" G_GSIZE_FORMAT " pending bytes",
                         gst_adapter_available(self->adapter));
        gst_adapter_clear(self->adapter);
    }

    gst_adapter_push(self->adapter, input);
    return GST_FLOW_OK;
}

/* Применяет режим VAD, если его поменяли из другого потока. */
static void sync_vad_mode(GstYadroVad *self, guint *hangover_frames) {
    GST_OBJECT_LOCK(self);
    gint mode = self->vad_mode;
    *hangover_frames = hangover_frames_for(self->hangover_ms);
    GST_OBJECT_UNLOCK(self);

    if (mode != self->applied_vad_mode) {
        if (fvad_set_mode(self->vad, mode) == 0) {
            GST_DEBUG_OBJECT(self, "vad-mode %d -> %d", self->applied_vad_mode, mode);
            self->applied_vad_mode = mode;
        }
    }
}

/*
 * GstBaseTransform вызывает generate_output в цикле, пока тот возвращает
 * буфер, и останавливается на первом NULL. Поэтому NULL можно вернуть
 * только тогда, когда в адаптере не осталось целого кадра. Кадры тишины
 * пропускаем внутри собственного цикла, иначе остаток входного буфера
 * застрянет в адаптере.
 */
static GstFlowReturn gst_yadro_vad_generate_output(GstBaseTransform *trans,
                                                   GstBuffer **outbuf) {
    GstYadroVad *self = GST_YADRO_VAD(trans);
    *outbuf = NULL;

    while (gst_adapter_available(self->adapter) >= YADRO_VAD_FRAME_BYTES) {
        guint hangover_frames;
        sync_vad_mode(self, &hangover_frames);

        GstBuffer *frame = gst_adapter_take_buffer(self->adapter, YADRO_VAD_FRAME_BYTES);

        GstMapInfo map;
        if (!gst_buffer_map(frame, &map, GST_MAP_READ)) {
            gst_buffer_unref(frame);
            GST_ELEMENT_ERROR(self, STREAM, FAILED, ("failed to map buffer"), (NULL));
            return GST_FLOW_ERROR;
        }
        int is_speech = fvad_process(self->vad, (const int16_t *)map.data, map.size / 2);
        gst_buffer_unmap(frame, &map);

        if (is_speech < 0) {
            gst_buffer_unref(frame);
            GST_ELEMENT_ERROR(self, STREAM, FAILED, ("fvad_process() failed"), (NULL));
            return GST_FLOW_ERROR;
        }

        /* Конечный автомат: речь → удержание на N кадров → тишина. */
        if (is_speech) {
            self->state = VAD_STATE_SPEECH;
            self->hangover_frames_left = hangover_frames;
        } else if (self->state != VAD_STATE_SILENCE) {
            if (self->hangover_frames_left > 0) {
                self->state = VAD_STATE_HANGOVER;
                self->hangover_frames_left--;
            } else {
                self->state = VAD_STATE_SILENCE;
            }
        }

        self->total_frames++;

        if (self->state == VAD_STATE_SILENCE) {
            self->frames_dropped++;
            g_string_append_c(self->mask, '0');
            self->dropped_since_last = TRUE;
            gst_buffer_unref(frame);
            continue;
        }

        self->frames_kept++;
        g_string_append_c(self->mask, '1');

        /* Буфер из адаптера может унаследовать флаги первого входного
         * буфера (в том числе DISCONT) — выставляем их сами. */
        frame = gst_buffer_make_writable(frame);
        GST_BUFFER_PTS(frame) = self->out_time;
        GST_BUFFER_DTS(frame) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(frame) = FRAME_DURATION;
        GST_BUFFER_OFFSET(frame) = GST_BUFFER_OFFSET_NONE;
        GST_BUFFER_OFFSET_END(frame) = GST_BUFFER_OFFSET_NONE;
        GST_BUFFER_FLAG_UNSET(frame, GST_BUFFER_FLAG_DISCONT);
        GST_BUFFER_FLAG_UNSET(frame, GST_BUFFER_FLAG_RESYNC);
        if (self->dropped_since_last && self->out_time > 0) {
            /* Склейка: между этим кадром и предыдущим сохранённым вырезана тишина. */
            GST_BUFFER_FLAG_SET(frame, GST_BUFFER_FLAG_DISCONT);
        }
        self->dropped_since_last = FALSE;
        self->out_time += FRAME_DURATION;

        *outbuf = frame;
        return GST_FLOW_OK;
    }

    return GST_FLOW_OK;
}

/* ------------------------------------------------------------------ события */

static void post_stats(GstYadroVad *self) {
    GstStructure *stats = gst_structure_new("YadroVadStats",
        "total_frames",   G_TYPE_UINT64, self->total_frames,
        "frames_kept",    G_TYPE_UINT64, self->frames_kept,
        "frames_dropped", G_TYPE_UINT64, self->frames_dropped,
        "frame_ms",       G_TYPE_UINT,   (guint)YADRO_VAD_FRAME_MS,
        "mask",           G_TYPE_STRING, self->mask->str,
        NULL);

    GST_INFO_OBJECT(self, "stats: total=%" G_GUINT64_FORMAT " kept=%" G_GUINT64_FORMAT
                    " dropped=%" G_GUINT64_FORMAT " mask=%s",
                    self->total_frames, self->frames_kept, self->frames_dropped,
                    self->mask->str);

    gst_element_post_message(GST_ELEMENT(self),
                             gst_message_new_element(GST_OBJECT(self), stats));
}

static gboolean gst_yadro_vad_sink_event(GstBaseTransform *trans, GstEvent *event) {
    GstYadroVad *self = GST_YADRO_VAD(trans);

    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_SEGMENT: {
        /* Элемент сжимает время, поэтому время на выходе — своё: сегмент
         * начинается с нуля, а PTS идут встык от out_time. Входной сегмент
         * (например, после перемотки он начинается не с нуля) вниз не
         * пропускаем, иначе приёмник отбросит наши буферы как «до начала». */
        const GstSegment *in_seg;
        gst_event_parse_segment(event, &in_seg);
        if (in_seg->format == GST_FORMAT_TIME) {
            GstSegment out_seg;
            gst_segment_init(&out_seg, GST_FORMAT_TIME);
            out_seg.base = self->out_time;
            out_seg.start = self->out_time;
            out_seg.position = self->out_time;
            out_seg.time = self->out_time;
            GstEvent *out_ev = gst_event_new_segment(&out_seg);
            gst_event_set_seqnum(out_ev, gst_event_get_seqnum(event));
            gst_event_unref(event);
            event = out_ev;
        }
        break;
    }
    case GST_EVENT_FLUSH_STOP:
        /* Перемотка: старые данные в адаптере больше не нужны, время на
         * выходе после flush снова начинается с нуля. */
        reset_stream_state(self);
        break;
    case GST_EVENT_EOS: {
        gsize tail = gst_adapter_available(self->adapter);
        if (tail > 0)
            GST_DEBUG_OBJECT(self, "EOS: dropping incomplete frame of %" G_GSIZE_FORMAT " bytes", tail);
        post_stats(self);
        reset_stream_state(self);
        reset_stats(self);
        break;
    }
    default:
        break;
    }

    return GST_BASE_TRANSFORM_CLASS(gst_yadro_vad_parent_class)->sink_event(trans, event);
}

/* ------------------------------------------------------------ регистрация */

static void gst_yadro_vad_class_init(GstYadroVadClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_yadro_vad_set_property;
    gobject_class->get_property = gst_yadro_vad_get_property;

    g_object_class_install_property(gobject_class, PROP_VAD_MODE,
        g_param_spec_int("vad-mode", "VAD mode",
            "VAD aggressiveness: 0 = quality, 1 = low bitrate, 2 = aggressive, 3 = very aggressive",
            0, 3, DEFAULT_VAD_MODE,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

    g_object_class_install_property(gobject_class, PROP_HANGOVER_TIME,
        g_param_spec_int("hangover-time", "Hangover time (ms)",
            "How long to keep passing audio after speech ends, rounded up to 30 ms frames",
            0, 2000, DEFAULT_HANGOVER_MS,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

    gst_element_class_add_static_pad_template(element_class, &src_template);
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_set_static_metadata(element_class,
        "Speech pause remover", "Filter/Effect/Audio",
        "Drops non-speech frames using WebRTC VAD and re-timestamps the rest back to back; "
        "posts a YadroVadStats element message on EOS",
        "ivanchez234 <https://github.com/ivanchez234>");

    trans_class->start = GST_DEBUG_FUNCPTR(gst_yadro_vad_start);
    trans_class->stop = GST_DEBUG_FUNCPTR(gst_yadro_vad_stop);
    trans_class->submit_input_buffer = GST_DEBUG_FUNCPTR(gst_yadro_vad_submit_input_buffer);
    trans_class->generate_output = GST_DEBUG_FUNCPTR(gst_yadro_vad_generate_output);
    trans_class->sink_event = GST_DEBUG_FUNCPTR(gst_yadro_vad_sink_event);
}

static void gst_yadro_vad_init(GstYadroVad *self) {
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
    self->vad_mode = DEFAULT_VAD_MODE;
    self->hangover_ms = DEFAULT_HANGOVER_MS;
}

static gboolean plugin_init(GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT(gst_yadro_vad_debug, "yadrovad", 0, "speech pause remover");
    return gst_element_register(plugin, "yadrovad", GST_RANK_NONE, GST_TYPE_YADRO_VAD);
}

#ifndef PACKAGE
#define PACKAGE "yadrovad"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, yadrovad,
                  "Speech pause remover based on WebRTC VAD (libfvad)",
                  plugin_init, "1.1.0", "MIT/X11", "yadrovad",
                  "https://github.com/ivanchez234/yadro_progect")
