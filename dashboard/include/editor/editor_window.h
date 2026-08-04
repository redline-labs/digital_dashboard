#ifndef DASHBOARD_EDITOR_EDITOR_WINDOW_H
#define DASHBOARD_EDITOR_EDITOR_WINDOW_H

#include <QMainWindow>

class QAction;
class WidgetPalette;
class Canvas;
class PropertiesPanel;

class EditorWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit EditorWindow(QWidget* parent = nullptr);
    ~EditorWindow() override = default;

    // Dialog-free load/save, so a config can be opened from the command line and
    // driven by the agent control interface. The menu actions are thin wrappers
    // that pick a path with QFileDialog and then call these.
    bool loadConfigFrom(const QString& path);
    bool saveConfigTo(const QString& path);

    Canvas* canvas() const { return canvas_; }

private:
    void buildMenuBar();
    void loadConfig();
    void saveConfig();

    WidgetPalette* widgetPalette_;
    Canvas* canvas_;
    QAction* toggleInterceptAction_;
};

#endif // DASHBOARD_EDITOR_EDITOR_WINDOW_H


