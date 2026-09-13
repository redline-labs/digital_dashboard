#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"

#include <QPoint>
#include <QWidget>
#include <vector>
#include <memory>

class QScreen;

class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg);
    ~MainWindow();

    // Get the window name for identification
    const std::string& getWindowName() const;

    // Full screen on `screen`, with the design-size layout centred in it.
    //
    // Scaling is not done here: the screen's scale factor (QT_SCREEN_SCALE_FACTORS,
    // set before QApplication) already makes its logical size close to the design
    // size, so what is left is the letterbox when the aspect ratios differ.
    void showOnScreen(QScreen* screen);

    // Replaces a live widget with one built from `cfg`, keeping its geometry,
    // objectName and position in the window. Dashboard widgets take their config
    // at construction and have no setter, so changing one means rebuilding it.
    // Returns false if `existing` is not one of this window's widgets.
    bool rebuildWidget(QWidget* existing, const widget_config_t& cfg);

  protected:
    void resizeEvent(QResizeEvent* event) override;

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

    // Where the design-size layout's (0, 0) sits in the window. Zero in a
    // design-size window; the letterbox offset once full screen.
    //
    // An offset rather than a fixed-size inner widget to hold the layout, because
    // that would put a level between MainWindow and its widgets and change every
    // agent selector path ("MainWindow/CarPlayWidget[0]") that has been written.
    QPoint _origin{0, 0};
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

