/*
 * yadrovad_cli — консольный запуск элемента yadrovad без GUI.
 *
 *   yadrovad_cli [опции] ФАЙЛ
 *
 *   --vad-mode N     агрессивность VAD 0..3 (по умолчанию 3)
 *   --hangover MS    удержание после речи, мс (по умолчанию 210)
 *   --tempo X        скорость воспроизведения 0.5..2.0 (нужен элемент pitch)
 *   --out FILE.wav   записать результат в WAV вместо воспроизведения
 *   --null           ничего не выводить в звук, только посчитать (быстро)
 *   --json           напечатать статистику и маску кадров в JSON (для стенда)
 *
 * Пути к файлам передаются в GStreamer как свойства/URI, а не через
 * текстовое описание конвейера, поэтому любые символы в именах безопасны.
 */
#include <gst/gst.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct Options {
    int vad_mode = 3;
    int hangover_ms = 210;
    double tempo = 1.0;
    std::string input;
    std::string out_wav;
    bool null_sink = false;
    bool json = false;
};

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s [--vad-mode N] [--hangover MS] [--tempo X] "
                 "[--out FILE.wav | --null] [--json] FILE\n",
                 argv0);
}

bool parse_args(int argc, char **argv, Options *o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--vad-mode") {
            const char *v = next("--vad-mode");
            if (!v) return false;
            o->vad_mode = std::atoi(v);
        } else if (a == "--hangover") {
            const char *v = next("--hangover");
            if (!v) return false;
            o->hangover_ms = std::atoi(v);
        } else if (a == "--tempo") {
            const char *v = next("--tempo");
            if (!v) return false;
            o->tempo = std::atof(v);
        } else if (a == "--out") {
            const char *v = next("--out");
            if (!v) return false;
            o->out_wav = v;
        } else if (a == "--null") {
            o->null_sink = true;
        } else if (a == "--json") {
            o->json = true;
        } else if (a == "-h" || a == "--help") {
            return false;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        } else {
            o->input = a;
        }
    }
    if (o->input.empty())
        return false;
    if (o->vad_mode < 0 || o->vad_mode > 3 || o->hangover_ms < 0 || o->hangover_ms > 2000 ||
        o->tempo < 0.1 || o->tempo > 4.0) {
        std::fprintf(stderr, "option value out of range\n");
        return false;
    }
    return true;
}

bool has_element(const char *name) {
    GstElementFactory *f = gst_element_factory_find(name);
    if (f)
        gst_object_unref(f);
    return f != nullptr;
}

bool ensure_plugin() {
    if (has_element("yadrovad"))
        return true;
#ifdef YADROVAD_PLUGIN_BUILD_DIR
    gst_registry_scan_path(gst_registry_get(), YADROVAD_PLUGIN_BUILD_DIR);
#endif
    return has_element("yadrovad");
}

GstElement *make_bin(const char *desc) {
    GError *err = nullptr;
    GstElement *bin = gst_parse_bin_from_description(desc, TRUE, &err);
    if (!bin) {
        std::fprintf(stderr, "failed to build '%s': %s\n", desc, err->message);
        g_clear_error(&err);
    }
    return bin;
}

}  // namespace

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }
    if (!ensure_plugin()) {
        std::fprintf(stderr, "element 'yadrovad' not found: set GST_PLUGIN_PATH to the plugin directory\n");
        return 1;
    }

    const bool use_pitch = opt.tempo != 1.0;
    if (use_pitch && !has_element("pitch")) {
        std::fprintf(stderr, "--tempo needs the 'pitch' element (gstreamer1.0-plugins-bad)\n");
        return 1;
    }

    GstElement *filter = make_bin(use_pitch
        ? "audioconvert ! audioresample ! yadrovad name=vad ! audioconvert ! audioresample ! "
          "pitch name=pitch ! audioconvert"
        : "audioconvert ! audioresample ! yadrovad name=vad");
    if (!filter)
        return 1;

    GstElement *sink = nullptr;
    if (!opt.out_wav.empty()) {
        sink = make_bin("audioconvert ! wavenc ! filesink name=file");
        if (!sink)
            return 1;
        GstElement *file = gst_bin_get_by_name(GST_BIN(sink), "file");
        g_object_set(file, "location", opt.out_wav.c_str(), NULL);
        gst_object_unref(file);
    } else if (opt.null_sink) {
        sink = gst_element_factory_make("fakesink", nullptr);
        g_object_set(sink, "sync", FALSE, NULL);
    }

    GError *err = nullptr;
    gchar *uri = gst_uri_is_valid(opt.input.c_str())
                     ? g_strdup(opt.input.c_str())
                     : gst_filename_to_uri(opt.input.c_str(), &err);
    if (!uri) {
        std::fprintf(stderr, "bad path: %s\n", err->message);
        g_clear_error(&err);
        return 1;
    }

    GstElement *vad = gst_bin_get_by_name(GST_BIN(filter), "vad");
    g_object_set(vad, "vad-mode", opt.vad_mode, "hangover-time", opt.hangover_ms, NULL);
    if (use_pitch) {
        GstElement *pitch = gst_bin_get_by_name(GST_BIN(filter), "pitch");
        g_object_set(pitch, "tempo", static_cast<gfloat>(opt.tempo), NULL);
        gst_object_unref(pitch);
    }

    GstElement *playbin = gst_element_factory_make("playbin", nullptr);
    if (!playbin) {
        std::fprintf(stderr, "playbin not found (gstreamer1.0-plugins-base)\n");
        return 1;
    }
    g_object_set(playbin, "uri", uri, "audio-filter", filter, "flags", 1 << 1 /* audio */, NULL);
    if (sink)
        g_object_set(playbin, "audio-sink", sink, NULL);
    g_free(uri);

    if (gst_element_set_state(playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "failed to start pipeline\n");
        return 1;
    }

    int rc = 0;
    bool done = false;
    guint64 total = 0, kept = 0, dropped = 0;
    std::string mask;
    bool have_stats = false;

    GstBus *bus = gst_element_get_bus(playbin);
    while (!done) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, GST_CLOCK_TIME_NONE,
            (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_ELEMENT));
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(vad) && s &&
                gst_structure_has_name(s, "YadroVadStats")) {
                gst_structure_get_uint64(s, "total_frames", &total);
                gst_structure_get_uint64(s, "frames_kept", &kept);
                gst_structure_get_uint64(s, "frames_dropped", &dropped);
                const char *m = gst_structure_get_string(s, "mask");
                mask = m ? m : "";
                have_stats = true;
            }
            break;
        }
        case GST_MESSAGE_ERROR: {
            GError *e = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            std::fprintf(stderr, "error from %s: %s\n%s\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
                         e->message, dbg ? dbg : "");
            g_clear_error(&e);
            g_free(dbg);
            rc = 1;
            done = true;
            break;
        }
        case GST_MESSAGE_EOS:
            done = true;
            break;
        default:
            break;
        }
        gst_message_unref(msg);
    }

    gst_element_set_state(playbin, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(vad);
    gst_object_unref(playbin);

    if (rc == 0 && !have_stats) {
        std::fprintf(stderr, "no YadroVadStats message received\n");
        rc = 1;
    }
    if (rc == 0) {
        if (opt.json) {
            std::printf("{\"total_frames\": %" G_GUINT64_FORMAT ", \"frames_kept\": %" G_GUINT64_FORMAT
                        ", \"frames_dropped\": %" G_GUINT64_FORMAT ", \"frame_ms\": 30, \"mask\": \"%s\"}\n",
                        total, kept, dropped, mask.c_str());
        } else {
            const double cut = total ? 100.0 * dropped / total : 0.0;
            std::printf("frames: total %" G_GUINT64_FORMAT ", kept %" G_GUINT64_FORMAT
                        ", dropped %" G_GUINT64_FORMAT " (%.1f%% removed); %.2f s -> %.2f s\n",
                        total, kept, dropped, cut, total * 0.03, kept * 0.03);
        }
    }
    return rc;
}
