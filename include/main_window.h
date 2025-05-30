#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"
#include "carplay_widget.h"

#include <QHBoxLayout>

// Forward declaration for FFmpeg
struct AVFrame;


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg);


  public slots:
    void update_carplay_image(AVFrame* frame);

  signals:
    void carplay_touch_event(TouchAction action, uint32_t x, uint32_t y);


  private:
    app_config_t _app_cfg;

    CarPlayWidget _carplay_widget;

    QHBoxLayout _horizontal_layout;

};  // class MainWindow


#endif  // MAIN_WINDOW_H_

