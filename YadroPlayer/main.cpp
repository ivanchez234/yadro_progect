#include "mainwindow.h"
#include <QApplication>
#include <gst/gst.h> // Подключаем GStreamer

int main(int argc, char *argv[])
{
    // Инициализируем GStreamer в самом начале!
    gst_init(&argc, &argv);

    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}
