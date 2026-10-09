#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include <atomic>

#include <gst/gst.h>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(bool pluginAvailable, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Открыть файл и сразу начать воспроизведение (путь из командной строки)
    void playFile(const QString &path);

private slots:
    void openFile();
    void togglePlay();
    void stop();
    void onTempoChanged(int value);
    void onVadModeChanged(int value);
    void onHangoverChanged(int value);

private:
    enum class State { Stopped, Playing, Paused };

    void setFile(const QString &path);
    bool buildPipeline();
    void destroyPipeline();
    void setState(State state);
    void handleBusMessage(GstMessage *msg, quint64 generation);
    void showStats(const GstStructure *s);
    void log(const QString &text);

    // Вызывается в потоке GStreamer: передаёт сообщение в поток GUI
    static GstBusSyncReply busSyncHandler(GstBus *bus, GstMessage *msg, gpointer self);

    Ui::MainWindow *ui;
    const bool pluginAvailable;

    GstElement *pipeline = nullptr;   // playbin
    GstElement *vadElement = nullptr;  // yadrovad внутри audio-filter
    GstElement *pitchElement = nullptr;
    State state = State::Stopped;
    // Номер текущего конвейера: сообщения от уже уничтоженного отбрасываются
    std::atomic<quint64> pipelineGeneration{0};
    QString currentFile;
};

#endif // MAINWINDOW_H
