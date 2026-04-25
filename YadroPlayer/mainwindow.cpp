#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QFileDialog>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , pipeline(nullptr)
    , isPlaying(false)
{
    ui->setupUi(this);
    ui->textLogs->append("✅ Система инициализирована. GStreamer готов.");
}

MainWindow::~MainWindow()
{
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    delete ui;
}

// === 1. ВЫБОР ФАЙЛА ===
void MainWindow::on_btnSelectFile_clicked()
{
    // Открываем диалоговое окно Ubuntu для выбора музыки
    QString fileName = QFileDialog::getOpenFileName(this, "Выберите аудиофайл", "", "Audio Files (*.mp3 *.wav)");

    if (!fileName.isEmpty()) {
        currentFile = fileName;
        ui->textLogs->append("📁 Выбран файл: " + currentFile);

        // Если уже играло что-то старое — останавливаем
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
            isPlaying = false;
            ui->btnPlay->setText("Play");
        }
    }
}

// === 2. КНОПКА PLAY / PAUSE ===
void MainWindow::on_btnPlay_clicked()
{
    if (currentFile.isEmpty()) {
        ui->textLogs->append("❌ Ошибка: Сначала выберите файл!");
        return;
    }

    if (!isPlaying) {
        // Если пайплайна еще нет — создаем его
        if (!pipeline) {
            // Читаем текущие значения ползунков
            float tempo = ui->sliderPitch->value() / 10.0f;
            int vadMode = ui->sliderVad->value();

            // МАГИЯ: Собираем пайплайн с ИМЕНАМИ элементов (name=pitch_elem и name=vad_elem)
            QString pipeline_str = QString(
                                       "filesrc location=\"%1\" ! decodebin ! audioconvert ! audioresample ! "
                                       "yadrovad name=vad_elem vad-mode=%2 hangover-time=200 ! "
                                       "audioconvert ! audioresample ! "
                                       "pitch name=pitch_elem tempo=%3 ! "
                                       "audioconvert ! autoaudiosink"
                                       ).arg(currentFile).arg(vadMode).arg(tempo);

            GError *error = nullptr;
            pipeline = gst_parse_launch(pipeline_str.toUtf8().constData(), &error);

            if (error) {
                ui->textLogs->append("❌ Ошибка сборки GStreamer: " + QString(error->message));
                g_clear_error(&error);
                return;
            }

            // =========================================================
            // === НОВОЕ: Подключаемся к шине сообщений GStreamer ======
            // =========================================================
            GstBus *bus = gst_element_get_bus(pipeline);
            gst_bus_add_signal_watch(bus); // Включаем отслеживание сигналов

            // Направляем сигналы "message" в нашу новую функцию handle_bus_message
            g_signal_connect_data(bus, "message", G_CALLBACK(+[](GstBus *bus, GstMessage *msg, gpointer data) {
                                      static_cast<MainWindow*>(data)->handle_bus_message(msg);
                                  }), this, NULL, (GConnectFlags)0);

            gst_object_unref(bus); // Очищаем указатель (сама "прослушка" останется)
            // =========================================================
        }

        // Запускаем звук
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        isPlaying = true;
        ui->btnPlay->setText("Pause");
        ui->textLogs->append("▶️ Воспроизведение...");

    } else {
        // Ставим на паузу
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        isPlaying = false;
        ui->btnPlay->setText("Play");
        ui->textLogs->append("⏸ Пауза");
    }
}

// === 3. ИЗМЕНЕНИЕ СКОРОСТИ НА ЛЕТУ ===
void MainWindow::on_sliderPitch_valueChanged(int value)
{
    float tempo = value / 10.0f; // Превращаем 15 в 1.5
    ui->textLogs->append(QString("⚡ Изменение скорости: x%1").arg(tempo));

    if (pipeline) {
        // Находим элемент pitch по имени и меняем параметр
        GstElement *pitch_elem = gst_bin_get_by_name(GST_BIN(pipeline), "pitch_elem");
        if (pitch_elem) {
            g_object_set(pitch_elem, "tempo", tempo, NULL);
            gst_object_unref(pitch_elem); // Обязательно освобождаем память
        }
    }
}

// === 4. ИЗМЕНЕНИЕ АГРЕССИВНОСТИ VAD НА ЛЕТУ ===
void MainWindow::on_sliderVad_valueChanged(int value)
{
    ui->textLogs->append(QString("🎙 Изменение агрессивности VAD: %1").arg(value));

    if (pipeline) {
        // Находим наш VAD-плагин по имени
        GstElement *vad_elem = gst_bin_get_by_name(GST_BIN(pipeline), "vad_elem");
        if (vad_elem) {
            g_object_set(vad_elem, "vad-mode", value, NULL);
            gst_object_unref(vad_elem);
        }
    }
}

void MainWindow::handle_bus_message(GstMessage *msg) {
    // Нас интересуют только сообщения от элементов (от нашего VAD)
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
        const GstStructure *s = gst_message_get_structure(msg);

        // Проверяем, что это именно наши данные телеметрии
        if (gst_structure_has_name(s, "YadroVadStats")) {
            guint64 total_frames, frames_kept, frames_dropped;

            // Вытаскиваем цифры
            gst_structure_get_uint64(s, "total_frames", &total_frames);
            gst_structure_get_uint64(s, "frames_kept", &frames_kept);
            gst_structure_get_uint64(s, "frames_dropped", &frames_dropped);

            // 1 кадр = 30 миллисекунд (0.03 секунды)
            double original_duration_sec = total_frames * 0.03;
            double final_duration_sec = frames_kept * 0.03;

            // Считаем процент сжатия
            double compression_percent = 0.0;
            if (total_frames > 0) {
                compression_percent = ((double)frames_dropped / total_frames) * 100.0;
            }

            // Выводим отчет в интерфейс (в textLogs)
            ui->textLogs->append("<br><b>📊 ОТЧЕТ VAD (Анализ файла завершен):</b>");
            ui->textLogs->append(QString("⏱ Исходная длительность: %1 сек.").arg(original_duration_sec, 0, 'f', 2));
            ui->textLogs->append(QString("⏱ Итоговая длительность: %1 сек.").arg(final_duration_sec, 0, 'f', 2));
            ui->textLogs->append(QString("✂️ Процент сжатия (вырезано тишины): %1%").arg(compression_percent, 0, 'f', 1));
            ui->textLogs->append("--------------------------------------------------");
        }
    }
}
