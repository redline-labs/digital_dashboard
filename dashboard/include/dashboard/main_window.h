#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"
//#include "carplay/carplay_widget.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_telltales/telltale.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "static_text/static_text.h"

#include <QWidget>
#include <vector>
#include <memory>
#include <map>

// Zenoh includes
#include "zenoh.hxx"

// Forward declaration for FFmpeg
struct AVFrame;


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const window_config_t& window_cfg);
    ~MainWindow();

    // Get the window name for identification
    const std::string& getWindowName() const;

  private:
    void createWidgetsFromConfig();
    QWidget* createWidget(const widget_config_t& widget_config);

    window_config_t _window_cfg;
    std::vector<std::unique_ptr<QWidget>> _widgets;
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

