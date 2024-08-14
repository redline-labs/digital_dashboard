#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"
#include "carplay_widget.h"

#include "QSparkLineWidget.h"

#include <QHBoxLayout>


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg);


  private:
    app_config_t _app_cfg;

    CarPlayWidget _carplay_widget;
    dqtx::QSparkLineWidget _sparkline_widget;

    QHBoxLayout _horizontal_layout;


};  // class MainWindow


#endif  // MAIN_WINDOW_H_

