#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"
#include "carplay/carplay_widget.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_telltales/battery_telltale.h"

#include <QWidget>
#include <vector>
#include <memory>

// Forward declaration for FFmpeg
struct AVFrame;


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg, bool libusb_debug = false);

    // Provide direct access to the CarPlay widget for integrated decoding
    CarPlayWidget* getCarPlayWidget();

  private:
    void createWidgetsFromConfig();
    QWidget* createWidget(const widget_config_t& widget_config);

    app_config_t _app_cfg;
    std::vector<std::unique_ptr<QWidget>> _widgets;
    CarPlayWidget* _carplay_widget; // Keep reference for external access
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

