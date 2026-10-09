#ifndef GST_YADRO_VAD_H
#define GST_YADRO_VAD_H

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/base/gstadapter.h>

extern "C" {
#include "fvad.h"
}

G_BEGIN_DECLS

/* Формат, с которым работает элемент: 16 кГц, моно, S16LE.
 * Один кадр VAD = 30 мс = 480 сэмплов = 960 байт. */
#define YADRO_VAD_RATE        16000
#define YADRO_VAD_FRAME_MS    30
#define YADRO_VAD_FRAME_BYTES (YADRO_VAD_RATE / 1000 * YADRO_VAD_FRAME_MS * 2)

typedef enum {
    VAD_STATE_SILENCE,  /* кадры выбрасываются */
    VAD_STATE_SPEECH,   /* последний кадр — речь */
    VAD_STATE_HANGOVER  /* речь кончилась, но ещё держим окно открытым */
} GstYadroVadState;

#define GST_TYPE_YADRO_VAD (gst_yadro_vad_get_type())
G_DECLARE_FINAL_TYPE(GstYadroVad, gst_yadro_vad, GST, YADRO_VAD, GstBaseTransform)

struct _GstYadroVad {
    GstBaseTransform element;

    /* --- Свойства. Пишутся из любого потока, защищены GST_OBJECT_LOCK --- */
    gint vad_mode;
    gint hangover_ms;

    /* --- Всё ниже трогает только потоковый поток (streaming thread) --- */
    GstAdapter *adapter;
    Fvad *vad;
    gint applied_vad_mode;         /* режим, реально выставленный в fvad */

    GstYadroVadState state;
    guint hangover_frames_left;

    GstClockTime out_time;         /* PTS следующего сохранённого кадра */
    gboolean dropped_since_last;   /* был вырез с момента последнего кадра → DISCONT */

    /* Телеметрия: отправляется сообщением YadroVadStats на EOS */
    guint64 total_frames;
    guint64 frames_kept;
    guint64 frames_dropped;
    GString *mask;                 /* '1' — кадр сохранён, '0' — выброшен */
};

G_END_DECLS

#endif /* GST_YADRO_VAD_H */
