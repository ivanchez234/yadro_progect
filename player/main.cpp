#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QStringList>

#include <gst/gst.h>

// Ищет элемент yadrovad. Если его нет в реестре (GST_PLUGIN_PATH не задан),
// пробует папки рядом с программой и папку сборки.
static bool ensureYadroVadPlugin()
{
    auto found = [] {
        GstElementFactory *f = gst_element_factory_find("yadrovad");
        if (f)
            gst_object_unref(f);
        return f != nullptr;
    };
    if (found())
        return true;

    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        qEnvironmentVariable("YADROVAD_PLUGIN_DIR"),
        appDir + "/../gst-plugins",            // build/player/YadroPlayer → build/gst-plugins
        appDir + "/gst-plugins",
        QStringLiteral(YADROVAD_PLUGIN_BUILD_DIR),
    };
    for (const QString &dir : candidates) {
        if (dir.isEmpty() || !QDir(dir).exists())
            continue;
        gst_registry_scan_path(gst_registry_get(), QDir(dir).absolutePath().toUtf8().constData());
        if (found())
            return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    QApplication app(argc, argv);

    MainWindow w(ensureYadroVadPlugin());
    w.show();

    // YadroPlayer файл.mp3 — открыть и сразу играть
    const QStringList args = QCoreApplication::arguments();
    if (args.size() > 1)
        w.playFile(args.at(1));

    return app.exec();
}
