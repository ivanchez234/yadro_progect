#ifndef __GST_YADRO_VAD_H__
#define __GST_YADRO_VAD_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/base/gstadapter.h>

extern "C" {
    #include "libfvad/include/fvad.h"
}

G_BEGIN_DECLS

typedef enum {
    VAD_STATE_SILENCE,
    VAD_STATE_SPEECH,
    VAD_STATE_HANGOVER
} GstYadroVadState;

#define GST_TYPE_YADRO_VAD (gst_yadro_vad_get_type())
G_DECLARE_FINAL_TYPE(GstYadroVad, gst_yadro_vad, GST, YADRO_VAD, GstBaseTransform)

struct _GstYadroVad {
    GstBaseTransform element;
    GstAdapter *adapter;
    Fvad *vad_inst;
    
    GstYadroVadState state;
    int hangover_time_left_ms;

    GstClockTime original_time;
    GstClockTime total_dropped_time;
    gboolean need_discont;

    /* --- НОВЫЕ ПЕРЕМЕННЫЕ ДЛЯ НАСТРОЕК --- */
    gint vad_mode;
    gint hangover_duration_ms;

    // === ТЕЛЕМЕТРИЯ (БЕНЧМАРК) ===
    guint64 total_frames;
    guint64 frames_kept;
    guint64 frames_dropped;
};

G_END_DECLS

#endif /* __GST_YADRO_VAD_H__ */