#include <QApplication>
#include <QSurfaceFormat>
#include <QCoreApplication>
#include <QGuiApplication>

#include "editor/editor_window.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    EditorWindow w;

    w.resize(1200, 800);
    w.show();

    return app.exec();
}


