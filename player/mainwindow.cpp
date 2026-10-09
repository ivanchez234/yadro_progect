#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>

namespace {

// Флаги playbin: только звук + программная громкость (видео игнорируем).
constexpr int kPlayFlagAudio = 1 << 1;
constexpr int kPlayFlagSoftVolume = 1 << 4;

constexpr int kDefaultVadMode = 2;
constexpr int kDefaultHangoverMs = 210;

bool hasElement(const char *name)
{
    GstElementFactory *f = gst_element_factory_find(name);
    if (f)
        gst_object_unref(f);
    return f != nullptr;
}

double tempoFromSlider(int value) { return value / 10.0; }

} // namespace

MainWindow::MainWindow(bool pluginAvailable, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , pluginAvailable(pluginAvailable)
{
    ui->setupUi(this);

    QFile qss(QStringLiteral(":/style.qss"));
    if (qss.open(QFile::ReadOnly | QFile::Text))
        setStyleSheet(QString::fromUtf8(qss.readAll()));

    ui->sliderVad->setValue(kDefaultVadMode);
    ui->spinHangover->setValue(kDefaultHangoverMs);
    onTempoChanged(ui->sliderTempo->value());
    onVadModeChanged(ui->sliderVad->value());

    connect(ui->btnOpen, &QPushButton::clicked, this, &MainWindow::openFile);
    connect(ui->btnPlay, &QPushButton::clicked, this, &MainWindow::togglePlay);
    connect(ui->btnStop, &QPushButton::clicked, this, &MainWindow::stop);
    connect(ui->sliderTempo, &QSlider::valueChanged, this, &MainWindow::onTempoChanged);
    connect(ui->sliderVad, &QSlider::valueChanged, this, &MainWindow::onVadModeChanged);
    connect(ui->spinHangover, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onHangoverChanged);

    if (!pluginAvailable) {
        log(tr("Ошибка: элемент GStreamer «yadrovad» не найден. Соберите плагин и укажите "
               "папку с ним в GST_PLUGIN_PATH (или YADROVAD_PLUGIN_DIR)."));
    }
    if (!hasElement("pitch")) {
        ui->sliderTempo->setEnabled(false);
        log(tr("Элемент «pitch» не найден — изменение скорости недоступно. "
               "Установите пакет gstreamer1.0-plugins-bad."));
    }

    setState(State::Stopped);
}

MainWindow::~MainWindow()
{
    destroyPipeline();
    delete ui;
}

// ------------------------------------------------------------------ действия

void MainWindow::openFile()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this, tr("Выберите аудиофайл"), QString(),
        tr("Аудио (*.wav *.mp3 *.flac *.ogg *.opus *.m4a);;Все файлы (*)"));
    if (!fileName.isEmpty())
        setFile(fileName);
}

void MainWindow::setFile(const QString &path)
{
    stop();
    currentFile = path;
    ui->labelFile->setText(QFileInfo(path).fileName());
    ui->labelFile->setToolTip(path);
    log(tr("Выбран файл: %1").arg(path));
    setState(State::Stopped);
}

void MainWindow::playFile(const QString &path)
{
    setFile(path);
    togglePlay();
}

void MainWindow::togglePlay()
{
    switch (state) {
    case State::Stopped:
        if (!buildPipeline())
            return;
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            // Конвейер не уничтожаем сразу: причина придёт сообщением ERROR
            // через очередь, его обработчик выведет текст и всё уберёт.
            log(tr("Не удалось запустить воспроизведение."));
            setState(State::Stopped);
            return;
        }
        log(tr("Воспроизведение"));
        setState(State::Playing);
        break;
    case State::Playing:
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        log(tr("Пауза"));
        setState(State::Paused);
        break;
    case State::Paused:
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        log(tr("Продолжение"));
        setState(State::Playing);
        break;
    }
}

void MainWindow::stop()
{
    if (!pipeline)
        return;
    destroyPipeline();
    log(tr("Остановлено"));
    setState(State::Stopped);
}

void MainWindow::onTempoChanged(int value)
{
    const double tempo = tempoFromSlider(value);
    ui->labelTempo->setText(tr("Скорость: x%1").arg(tempo, 0, 'f', 1));
    if (pitchElement)
        g_object_set(pitchElement, "tempo", static_cast<gfloat>(tempo), NULL);
}

void MainWindow::onVadModeChanged(int value)
{
    static const char *const names[] = {"мягко", "умеренно", "агрессивно", "очень агрессивно"};
    ui->labelVad->setText(tr("Агрессивность VAD: %1 (%2)").arg(value).arg(tr(names[value & 3])));
    if (vadElement)
        g_object_set(vadElement, "vad-mode", value, NULL);
}

void MainWindow::onHangoverChanged(int value)
{
    if (vadElement)
        g_object_set(vadElement, "hangover-time", value, NULL);
}

// ------------------------------------------------------------------ конвейер

bool MainWindow::buildPipeline()
{
    destroyPipeline();  // остатки неудачного запуска, если были

    if (currentFile.isEmpty()) {
        log(tr("Сначала выберите файл."));
        return false;
    }
    if (!pluginAvailable) {
        log(tr("Воспроизведение невозможно: нет элемента «yadrovad»."));
        return false;
    }

    // Описание фильтра фиксированное — пользовательских данных в нём нет.
    // Путь к файлу передаётся отдельно, как URI, поэтому кавычки и пробелы
    // в имени файла конвейер не ломают.
    const bool withPitch = hasElement("pitch");
    const char *filterDesc = withPitch
        ? "audioconvert ! audioresample ! yadrovad name=vad ! "
          "audioconvert ! audioresample ! pitch name=pitch ! audioconvert"
        : "audioconvert ! audioresample ! yadrovad name=vad ! audioconvert";

    GError *err = nullptr;
    GstElement *filter = gst_parse_bin_from_description(filterDesc, TRUE, &err);
    if (!filter) {
        log(tr("Ошибка сборки фильтра: %1").arg(QString::fromUtf8(err->message)));
        g_clear_error(&err);
        return false;
    }

    pipeline = gst_element_factory_make("playbin", "player");
    if (!pipeline) {
        gst_object_unref(filter);
        log(tr("Не найден элемент playbin (пакет gstreamer1.0-plugins-base)."));
        return false;
    }

    gchar *uri = gst_filename_to_uri(QFile::encodeName(currentFile).constData(), &err);
    if (!uri) {
        log(tr("Некорректный путь: %1").arg(QString::fromUtf8(err->message)));
        g_clear_error(&err);
        gst_object_unref(filter);
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return false;
    }

    vadElement = gst_bin_get_by_name(GST_BIN(filter), "vad");
    pitchElement = withPitch ? gst_bin_get_by_name(GST_BIN(filter), "pitch") : nullptr;

    g_object_set(pipeline,
                 "uri", uri,
                 "audio-filter", filter,  // playbin забирает floating-ссылку
                 "flags", kPlayFlagAudio | kPlayFlagSoftVolume,
                 NULL);
    g_free(uri);

    // Текущие значения ползунков
    g_object_set(vadElement,
                 "vad-mode", ui->sliderVad->value(),
                 "hangover-time", ui->spinHangover->value(),
                 NULL);
    if (pitchElement)
        g_object_set(pitchElement, "tempo",
                     static_cast<gfloat>(tempoFromSlider(ui->sliderTempo->value())), NULL);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, &MainWindow::busSyncHandler, this, nullptr);
    gst_object_unref(bus);
    return true;
}

void MainWindow::destroyPipeline()
{
    if (!pipeline)
        return;

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
    gst_object_unref(bus);

    // Переход в NULL синхронный: после него потоки GStreamer остановлены.
    gst_element_set_state(pipeline, GST_STATE_NULL);

    g_clear_pointer(&vadElement, gst_object_unref);
    g_clear_pointer(&pitchElement, gst_object_unref);
    g_clear_pointer(&pipeline, gst_object_unref);
    ++pipelineGeneration;
}

void MainWindow::setState(State newState)
{
    state = newState;
    ui->btnPlay->setText(state == State::Playing ? tr("Пауза") : tr("Играть"));
    ui->btnPlay->setEnabled(pluginAvailable && !currentFile.isEmpty());
    ui->btnStop->setEnabled(state != State::Stopped);
    statusBar()->showMessage(state == State::Playing  ? tr("Воспроизведение")
                             : state == State::Paused ? tr("Пауза")
                                                      : tr("Остановлено"));
}

// ------------------------------------------------------------------ шина

// Шина GStreamer вызывает обработчик в своём потоке. Интерфейс трогать
// оттуда нельзя, поэтому нужные сообщения перекладываются в поток GUI через
// очередь событий Qt. Так не нужен цикл GLib, и плеер не зависит от того,
// на каком диспетчере событий собран Qt.
GstBusSyncReply MainWindow::busSyncHandler(GstBus *, GstMessage *msg, gpointer self)
{
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
    case GST_MESSAGE_ERROR:
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ELEMENT: {
        auto *w = static_cast<MainWindow *>(self);
        const quint64 generation = w->pipelineGeneration.load();
        gst_message_ref(msg);
        QMetaObject::invokeMethod(w, [w, msg, generation] {
            w->handleBusMessage(msg, generation);
            gst_message_unref(msg);
        }, Qt::QueuedConnection);
        break;
    }
    default:
        break;
    }
    return GST_BUS_DROP;
}

void MainWindow::handleBusMessage(GstMessage *msg, quint64 generation)
{
    // Сообщение могло прийти от уже уничтоженного конвейера.
    if (!pipeline || generation != pipelineGeneration.load())
        return;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ELEMENT: {
        const GstStructure *s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "YadroVadStats"))
            showStats(s);
        break;
    }
    case GST_MESSAGE_EOS:
        log(tr("Файл доигран до конца"));
        destroyPipeline();
        setState(State::Stopped);
        break;
    case GST_MESSAGE_ERROR:
    case GST_MESSAGE_WARNING: {
        GError *err = nullptr;
        gchar *debug = nullptr;
        const bool isError = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
        if (isError)
            gst_message_parse_error(msg, &err, &debug);
        else
            gst_message_parse_warning(msg, &err, &debug);
        log(QStringLiteral("%1 (%2): %3")
                .arg(isError ? tr("Ошибка") : tr("Предупреждение"),
                     QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg))),
                     QString::fromUtf8(err ? err->message : "?")));
        if (debug)
            log(QString::fromUtf8(debug));
        g_clear_error(&err);
        g_free(debug);
        if (isError) {
            destroyPipeline();
            setState(State::Stopped);
        }
        break;
    }
    default:
        break;
    }
}

void MainWindow::showStats(const GstStructure *s)
{
    guint64 total = 0, kept = 0, dropped = 0;
    guint frameMs = 30;
    gst_structure_get_uint64(s, "total_frames", &total);
    gst_structure_get_uint64(s, "frames_kept", &kept);
    gst_structure_get_uint64(s, "frames_dropped", &dropped);
    gst_structure_get_uint(s, "frame_ms", &frameMs);

    const double original = total * frameMs / 1000.0;
    const double result = kept * frameMs / 1000.0;
    const double cut = total ? 100.0 * dropped / total : 0.0;

    log(tr("Итог: было %1 с, стало %2 с, вырезано пауз %3% (%4 из %5 кадров)")
            .arg(original, 0, 'f', 1)
            .arg(result, 0, 'f', 1)
            .arg(cut, 0, 'f', 1)
            .arg(dropped)
            .arg(total));
}

void MainWindow::log(const QString &text)
{
    qInfo().noquote() << text;  // дублируем в консоль: удобно при запуске из терминала
    ui->textLogs->append(QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                  text.toHtmlEscaped()));
}
