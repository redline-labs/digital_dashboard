#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"
#include "carplay/carplay_widget.h"

#include <QHBoxLayout>

// Forward declaration for FFmpeg
struct AVFrame;


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg, bool libusb_debug = false);

    // Provide direct access to the CarPlay widget for integrated decoding
    CarPlayWidget& getCarPlayWidget() { return _carplay_widget; }


  private:
    app_config_t _app_cfg;

    CarPlayWidget _carplay_widget;

    QHBoxLayout _horizontal_layout;

};  // class MainWindow


#endif  // MAIN_WINDOW_H_

