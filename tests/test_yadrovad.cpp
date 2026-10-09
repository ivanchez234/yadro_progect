/*
 * Тесты элемента yadrovad через GstHarness.
 *
 * Сигнал синтетический: тишина + три «фразы» (гармоники 140 Гц с формантами
 * и слоговой огибающей), которые libfvad уверенно считает речью, разделённые
 * цифровой тишиной. Вход подаётся буферами по 4096 байт — так режет wavparse,
 * и именно на таком размере проявлялась потеря кадров.
 *
 * Запуск: test_yadrovad <имя_теста>   (без аргумента — все тесты)
 */
#include <gst/gst.h>
#include <gst/check/gstharness.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kRate = 16000;
constexpr size_t kFrameBytes = 960;
constexpr GstClockTime kFrameDur = 30 * GST_MSECOND;
constexpr const char *kCaps =
    "audio/x-raw,format=S16LE,rate=16000,channels=1,layout=interleaved";

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        auto va_ = (a);                                                          \
        auto vb_ = (b);                                                          \
        if (!(va_ == vb_)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n",       \
                         __FILE__, __LINE__, #a, #b, (long long)va_, (long long)vb_); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// ------------------------------------------------------------------ сигнал

void append_silence(std::vector<int16_t> &s, double sec) {
    s.insert(s.end(), size_t(sec * kRate), 0);
}

void append_voice(std::vector<int16_t> &s, double sec) {
    const double f0 = 140.0;
    const size_t n = size_t(sec * kRate);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        double v = 0;
        for (int k = 1; k <= 25; ++k) {
            const double f = k * f0;
            const double formants = std::exp(-std::pow((f - 700) / 300, 2)) +
                                    0.6 * std::exp(-std::pow((f - 1200) / 400, 2)) +
                                    0.3 * std::exp(-std::pow((f - 2500) / 500, 2));
            v += formants * std::sin(2 * M_PI * f * t + k);
        }
        const double env = 0.6 + 0.4 * std::sin(2 * M_PI * 4 * t);
        s.push_back(int16_t(std::lround(4000 * env * v)));
    }
}

// 0.6 с тишины, три фразы по 0.9 с через паузы 1.5 с, 1.0 с тишины в конце.
std::vector<int16_t> three_phrases() {
    std::vector<int16_t> s;
    append_silence(s, 0.6);
    for (int i = 0; i < 3; ++i) {
        append_voice(s, 0.9);
        append_silence(s, i < 2 ? 1.5 : 1.0);
    }
    return s;
}

// ------------------------------------------------------------------ прогон

struct Stats {
    bool received = false;
    guint64 total = 0, kept = 0, dropped = 0;
    std::string mask;
};

struct RunResult {
    std::vector<GstClockTime> pts;
    std::vector<GstClockTime> dur;
    std::vector<bool> discont;
    size_t out_bytes = 0;
    Stats stats;
};

GstHarness *make_harness(int vad_mode, int hangover_ms) {
    GstHarness *h = gst_harness_new("yadrovad");
    g_object_set(h->element, "vad-mode", vad_mode, "hangover-time", hangover_ms, NULL);
    gst_harness_set_src_caps_str(h, kCaps);
    return h;
}

// Первый буфер помечается DISCONT, как это делает wavparse/декодер.
void push_samples(GstHarness *h, const std::vector<int16_t> &s, size_t chunk_bytes,
                  GstClockTime *ts, bool first_discont = true) {
    const uint8_t *data = reinterpret_cast<const uint8_t *>(s.data());
    const size_t total = s.size() * sizeof(int16_t);
    for (size_t off = 0; off < total; off += chunk_bytes) {
        const size_t len = std::min(chunk_bytes, total - off);
        GstBuffer *buf = gst_buffer_new_memdup(data + off, len);
        if (off == 0 && first_discont)
            GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DISCONT);
        GST_BUFFER_PTS(buf) = *ts;
        GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(len / 2, GST_SECOND, kRate);
        *ts += GST_BUFFER_DURATION(buf);
        g_assert(gst_harness_push(h, buf) == GST_FLOW_OK);
    }
}

void collect_output(GstHarness *h, RunResult *r) {
    GstBuffer *buf;
    while ((buf = gst_harness_try_pull(h)) != nullptr) {
        r->pts.push_back(GST_BUFFER_PTS(buf));
        r->dur.push_back(GST_BUFFER_DURATION(buf));
        r->discont.push_back(GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DISCONT));
        r->out_bytes += gst_buffer_get_size(buf);
        gst_buffer_unref(buf);
    }
}

// Сообщение YadroVadStats уходит на шину элемента; харнесс шину не создаёт,
// поэтому ставим свою.
Stats read_stats(GstBus *bus) {
    Stats st;
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ELEMENT)) != nullptr) {
        const GstStructure *s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "YadroVadStats")) {
            st.received = true;
            gst_structure_get_uint64(s, "total_frames", &st.total);
            gst_structure_get_uint64(s, "frames_kept", &st.kept);
            gst_structure_get_uint64(s, "frames_dropped", &st.dropped);
            st.mask = gst_structure_get_string(s, "mask");
        }
        gst_message_unref(msg);
    }
    return st;
}

RunResult run(const std::vector<int16_t> &signal, int vad_mode, int hangover_ms,
              size_t chunk_bytes = 4096) {
    GstHarness *h = make_harness(vad_mode, hangover_ms);
    GstBus *bus = gst_bus_new();
    gst_element_set_bus(h->element, bus);

    RunResult r;
    GstClockTime ts = 0;
    push_samples(h, signal, chunk_bytes, &ts);
    gst_harness_push_event(h, gst_event_new_eos());
    collect_output(h, &r);
    r.stats = read_stats(bus);

    gst_element_set_bus(h->element, nullptr);
    gst_object_unref(bus);
    gst_harness_teardown(h);
    return r;
}

size_t count_runs_of_ones(const std::string &mask) {
    size_t runs = 0;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i] == '1' && (i == 0 || mask[i - 1] == '0'))
            ++runs;
    return runs;
}

// ------------------------------------------------------------------ тесты

// Регрессия: раньше generate_output возвращал NULL на кадре тишины, и
// остаток входного буфера терялся. Теперь каждый целый кадр входа должен
// быть учтён и сохранённые кадры должны дойти до выхода.
void test_all_frames_processed() {
    const auto sig = three_phrases();
    const guint64 expected_frames = sig.size() * 2 / kFrameBytes;

    for (size_t chunk : {size_t(4096), size_t(960), size_t(100), size_t(16000)}) {
        const RunResult r = run(sig, 3, 210, chunk);
        CHECK(r.stats.received);
        CHECK_EQ(r.stats.total, expected_frames);
        CHECK_EQ(r.stats.kept + r.stats.dropped, r.stats.total);
        CHECK_EQ(r.stats.mask.size(), size_t(r.stats.total));
        CHECK_EQ(r.pts.size(), size_t(r.stats.kept));
        CHECK_EQ(r.out_bytes, size_t(r.stats.kept) * kFrameBytes);
    }
}

// Все три фразы должны дойти до выхода (раньше 2-я и 3-я пропадали).
void test_three_phrases_survive() {
    const RunResult r = run(three_phrases(), 3, 210);
    CHECK_EQ(count_runs_of_ones(r.stats.mask), size_t(3));
    // Каждая фраза 0.9 с = 30 кадров; на выходе не меньше 3 × 30 кадров.
    CHECK(r.stats.kept >= 90);
    // И тишины выброшено много: паузы 0.6 + 1.5 + 1.5 + 1.0 = 4.6 с ≈ 153 кадра.
    CHECK(r.stats.dropped >= 100);
}

// PTS идут встык от нуля, длительность каждого буфера 30 мс.
void test_pts_contiguous() {
    const RunResult r = run(three_phrases(), 3, 210);
    CHECK(!r.pts.empty());
    for (size_t i = 0; i < r.pts.size(); ++i) {
        CHECK_EQ(r.pts[i], GstClockTime(i * kFrameDur));
        CHECK_EQ(r.dur[i], kFrameDur);
    }
}

// DISCONT: на первом выходном буфере (начало потока, вход начинался с
// DISCONT) и на каждой склейке: 3 фразы → 2 склейки, всего 3 флага.
// Кадры, нарезанные из одного входного буфера, флаг не наследуют.
void test_discont_only_on_splices() {
    const RunResult r = run(three_phrases(), 3, 210);
    size_t discont = 0;
    for (bool d : r.discont)
        discont += d ? 1 : 0;
    CHECK_EQ(discont, size_t(3));
    CHECK(r.discont.size() > 2 && r.discont[0] && !r.discont[1] && !r.discont[2]);

    // Без DISCONT на входе остаются только 2 склейки.
    GstHarness *h = make_harness(3, 210);
    GstClockTime ts = 0;
    push_samples(h, three_phrases(), 4096, &ts, false);
    gst_harness_push_event(h, gst_event_new_eos());
    RunResult plain;
    collect_output(h, &plain);
    gst_harness_teardown(h);
    discont = 0;
    for (bool d : plain.discont)
        discont += d ? 1 : 0;
    CHECK_EQ(discont, size_t(2));
}

// Разрыв во входе посреди кадра: недобранный хвост выбрасывается, а не
// склеивается с данными после разрыва. Ложных разрывов (DISCONT только у
// первого буфера) это не касается — ни один кадр не теряется.
void test_input_discont_drops_partial_frame() {
    const auto sig = three_phrases();
    GstHarness *h = make_harness(3, 210);
    GstBus *bus = gst_bus_new();
    gst_element_set_bus(h->element, bus);

    GstClockTime ts = 0;
    std::vector<int16_t> head(sig.begin(), sig.begin() + 250);  // 500 байт < кадра
    push_samples(h, head, 4096, &ts);
    push_samples(h, sig, 4096, &ts);  // начинается с DISCONT
    gst_harness_push_event(h, gst_event_new_eos());

    RunResult r;
    collect_output(h, &r);
    r.stats = read_stats(bus);
    CHECK_EQ(r.stats.total, guint64(sig.size() * 2 / kFrameBytes));

    gst_element_set_bus(h->element, nullptr);
    gst_object_unref(bus);
    gst_harness_teardown(h);
}

// Удержание: решения fvad не зависят от hangover, значит разница в числе
// сохранённых кадров — ровно N кадров удержания на каждую из 3 фраз.
// 210 мс → 7 кадров (округление вверх), 200 мс → 7, 180 мс → 6, 0 → 0.
void test_hangover_frames() {
    const auto sig = three_phrases();
    const guint64 base = run(sig, 3, 0).stats.kept;
    CHECK_EQ(run(sig, 3, 210).stats.kept - base, guint64(3 * 7));
    CHECK_EQ(run(sig, 3, 200).stats.kept - base, guint64(3 * 7));
    CHECK_EQ(run(sig, 3, 180).stats.kept - base, guint64(3 * 6));
    CHECK_EQ(run(sig, 3, 30).stats.kept - base, guint64(3 * 1));
}

// Поток без речи: всё выброшено, на выход ничего не уходит.
void test_silence_only() {
    std::vector<int16_t> sig;
    append_silence(sig, 6.4);
    const RunResult r = run(sig, 3, 210);
    CHECK_EQ(r.stats.total, guint64(sig.size() * 2 / kFrameBytes));
    CHECK_EQ(r.stats.kept, guint64(0));
    CHECK(r.pts.empty());
}

// Перемотка: после FLUSH недобранный хвост выбрасывается, время на выходе
// начинается заново с нуля, а входной сегмент заменяется своим (с нуля),
// иначе приёмник выбросил бы буферы как лежащие до начала сегмента.
void test_flush_and_segment() {
    GstHarness *h = make_harness(3, 210);
    const auto sig = three_phrases();

    // Кусок, не кратный кадру: 500 байт останутся в адаптере.
    std::vector<int16_t> head(sig.begin(), sig.begin() + 250);
    GstClockTime ts = 0;
    push_samples(h, head, 4096, &ts);

    gst_harness_push_event(h, gst_event_new_flush_start());
    gst_harness_push_event(h, gst_event_new_flush_stop(TRUE));
    GstSegment seg;
    gst_segment_init(&seg, GST_FORMAT_TIME);
    seg.start = seg.time = seg.position = 10 * GST_SECOND;
    gst_harness_push_event(h, gst_event_new_segment(&seg));

    // Сегмент, ушедший вниз после flush, должен начинаться с нуля.
    bool saw_segment = false;
    GstEvent *ev;
    while ((ev = gst_harness_try_pull_event(h)) != nullptr) {
        if (GST_EVENT_TYPE(ev) == GST_EVENT_SEGMENT) {
            const GstSegment *out;
            gst_event_parse_segment(ev, &out);
            if (out->start == 0 && out->time == 0 && out->base == 0)
                saw_segment = true;  // последний сегмент — наш
            else
                saw_segment = false;
        }
        gst_event_unref(ev);
    }
    CHECK(saw_segment);

    GstBus *bus = gst_bus_new();
    gst_element_set_bus(h->element, bus);
    ts = 10 * GST_SECOND;
    push_samples(h, sig, 4096, &ts);
    gst_harness_push_event(h, gst_event_new_eos());

    RunResult r;
    collect_output(h, &r);
    r.stats = read_stats(bus);
    CHECK(!r.pts.empty() && r.pts.front() == 0);
    // Хвост в 500 байт до flush не должен был склеиться с новыми данными:
    // кадров столько же, сколько в чистом прогоне того же сигнала
    // (плюс 0 кадров от головы — она короче одного кадра).
    CHECK_EQ(r.stats.total, guint64(sig.size() * 2 / kFrameBytes));

    gst_element_set_bus(h->element, nullptr);
    gst_object_unref(bus);
    gst_harness_teardown(h);
}

// Свойства меняются из другого потока во время обработки (так делает
// ползунок в плеере). Проверяем, что это не ломает поток и что новое
// значение применяется.
void test_property_change_while_streaming() {
    GstHarness *h = make_harness(0, 210);
    const auto sig = three_phrases();
    std::atomic<bool> stop{false};
    std::thread gui([&] {
        int m = 0;
        while (!stop.load()) {
            g_object_set(h->element, "vad-mode", (m++) % 4, NULL);
            std::this_thread::yield();
        }
    });
    GstClockTime ts = 0;
    for (int i = 0; i < 5; ++i)
        push_samples(h, sig, 4096, &ts);
    stop = true;
    gui.join();

    g_object_set(h->element, "vad-mode", 2, NULL);
    int mode = -1;
    g_object_get(h->element, "vad-mode", &mode, NULL);
    CHECK_EQ(mode, 2);

    RunResult r;
    collect_output(h, &r);
    CHECK(!r.pts.empty());
    gst_harness_teardown(h);
}

struct Test {
    const char *name;
    std::function<void()> fn;
};

const Test kTests[] = {
    {"all_frames_processed", test_all_frames_processed},
    {"three_phrases_survive", test_three_phrases_survive},
    {"pts_contiguous", test_pts_contiguous},
    {"discont_only_on_splices", test_discont_only_on_splices},
    {"hangover_frames", test_hangover_frames},
    {"silence_only", test_silence_only},
    {"input_discont_drops_partial_frame", test_input_discont_drops_partial_frame},
    {"flush_and_segment", test_flush_and_segment},
    {"property_change_while_streaming", test_property_change_while_streaming},
};

}  // namespace

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    GstElementFactory *factory = gst_element_factory_find("yadrovad");
    if (!factory) {
        std::fprintf(stderr, "element 'yadrovad' not found; set GST_PLUGIN_PATH\n");
        return 2;
    }
    gst_object_unref(factory);

    const char *only = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const Test &t : kTests) {
        if (only && std::strcmp(only, t.name) != 0)
            continue;
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "unknown test: %s\n", only);
        return 2;
    }
    return g_failures == 0 ? 0 : 1;
}
