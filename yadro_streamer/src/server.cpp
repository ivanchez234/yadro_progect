#include <gst/gst.h>
#include <iostream>
#include <string>

// В Linux нам не нужны заголовочные файлы Windows и сложные макросы для сети.
// Весь код теперь работает в одном главном потоке.

int main(int argc, char *argv[]) {
    // 1. Проверка аргументов командной строки
    if (argc < 3) {
        std::cout << "❌ Ошибка: Недостаточно параметров!" << std::endl;
        std::cout << "Использование: ./yadro_server <путь_к_файлу> <ускорение>" << std::endl;
        std::cout << "Пример: ./yadro_server podcast.mp3 1.5" << std::endl;
        return -1;
    }

    std::string filePath = argv[1];
    std::string tempo = argv[2];

    // 2. Инициализация GStreamer
    gst_init(&argc, &argv);

    std::cout << "==== YADRO LOCAL PLAYER (UBUNTU) ====" << std::endl;
    std::cout << "📁 Файл: " << filePath << std::endl;
    std::cout << "⚡ Скорость: x" << tempo << std::endl;

    // 3. Сборка пайплайна. 
    // Вместо appsink мы используем autoaudiosink — он сам найдет твои колонки или наушники.
    std::string pipeline_str = 
        "filesrc location=\"" + filePath + "\" ! "
        "decodebin ! audioconvert ! audioresample ! "
        "yadrovad vad-mode=3 hangover-time=200 ! " // Твой VAD плагин
        "audioconvert ! audioresample ! "
        "pitch tempo=" + tempo + " ! "             // Ускоритель
        "audioconvert ! autoaudiosink";            // Выход на динамики

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);

    if (error) {
        std::cerr << "❌ Ошибка GStreamer: " << error->message << std::endl;
        g_clear_error(&error);
        return -1;
    }

    // 4. Запуск воспроизведения
    std::cout << "▶️ Воспроизведение..." << std::endl;
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // 5. Ожидание завершения или ошибки
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    if (msg != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err; gchar *debug;
            gst_message_parse_error(msg, &err, &debug);
            std::cerr << "❌ Критическая ошибка: " << err->message << std::endl;
            g_clear_error(&err); g_free(debug);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            std::cout << "✅ Воспроизведение успешно завершено." << std::endl;
        }
        gst_message_unref(msg);
    }

    // 6. Освобождение ресурсов
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(bus);

    return 0;
}