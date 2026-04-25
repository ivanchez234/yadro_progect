#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <gst/gst.h>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // Это наши слоты (реакции на действия интерфейса)
    // Важно: названия должны строго совпадать с именами объектов в UI!
    void on_btnSelectFile_clicked();
    void on_btnPlay_clicked();
    void on_sliderPitch_valueChanged(int value);
    void on_sliderVad_valueChanged(int value);

private:
    Ui::MainWindow *ui;

    // === АРХИТЕКТУРА ЯДРА ===
    GstElement *pipeline; // Указатель на GStreamer
    bool isPlaying;       // Состояние плеера
    QString currentFile;  // Путь к выбранному MP3/WAV
};
#endif // MAINWINDOW_H
