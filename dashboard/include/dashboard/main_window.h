#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"

#include <QWidget>
#include <vector>
#include <memory>


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg);
    ~MainWindow();

    // Get the window name for identification
    const std::string& getWindowName() const;

    // Replaces a live widget with one built from `cfg`, keeping its geometry,
    // objectName and position in the window. Dashboard widgets take their config
    // at construction and have no setter, so changing one means rebuilding it.
    // Returns false if `existing` is not one of this window's widgets.
    bool rebuildWidget(QWidget* existing, const widget_config_t& cfg);

  private:
    void createWidgetsFromConfig();

    app_config_t _app_cfg;
    std::vector<std::unique_ptr<QWidget>> _widgets;
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

