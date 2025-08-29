#ifndef DASHBOARD_EDITOR_EDITOR_WINDOW_H
#define DASHBOARD_EDITOR_EDITOR_WINDOW_H

#include <QMainWindow>

class QAction;
class WidgetPalette;
class Canvas;

class EditorWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit EditorWindow(QWidget* parent = nullptr);
    ~EditorWindow() override = default;

private:
    WidgetPalette* widgetPalette_;
    Canvas* canvas_;
    QAction* toggleInterceptAction_;
};

#endif // DASHBOARD_EDITOR_EDITOR_WINDOW_H


