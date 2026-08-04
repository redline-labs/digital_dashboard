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

    // A live widget and the index of the config entry it was built from. The
    // two are not the same number: a widget that fails to construct is skipped
    // here but kept in _app_cfg.widgets, so after one failure the positions
    // drift apart and rebuildWidget() wrote a new config over its neighbour.
    struct LiveWidget
    {
        std::unique_ptr<QWidget> widget;
        std::size_t config_index;
    };

    std::vector<LiveWidget> _widgets;
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

