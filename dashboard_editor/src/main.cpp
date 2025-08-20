#include <QApplication>
#include <QSurfaceFormat>

#include "editor_window.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    EditorWindow w;
    w.resize(1200, 800);
    w.show();

    return app.exec();
}


