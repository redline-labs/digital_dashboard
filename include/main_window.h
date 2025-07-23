#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"
#include "carplay/carplay_widget.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_telltales/battery_telltale.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"

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
    MainWindow(const app_config_t& app_cfg, const window_config_t& window_cfg);
    ~MainWindow();

    // Get the window name for identification
    const std::string& getWindowName() const;

  private slots:
    // Zenoh data reception slots (thread-safe via Qt's queued connections)
    void onSpeedDataReceived(double speedMps);
    void onRpmDataReceived(double rpm);
    void onSparklineDataReceived(double value);
    void onBatteryTelltaleDataReceived(bool asserted);

  private:
    void createWidgetsFromConfig();
    QWidget* createWidget(const widget_config_t& widget_config);
    void initializeZenoh();
    void createZenohSubscription(const std::string& zenoh_key, const std::string& widget_type);

    app_config_t _app_cfg;
    window_config_t _window_cfg;
    std::vector<std::unique_ptr<QWidget>> _widgets;

    // Zenoh-related members
    std::unique_ptr<zenoh::Session> _zenoh_session;
    std::vector<std::unique_ptr<zenoh::Subscriber<void>>> _zenoh_subscribers;
    
    // Widget mappings for data updates
    std::map<std::string, Mercedes190ESpeedometer*> _speedometer_widgets;
    std::map<std::string, TachometerWidget*> _tachometer_widgets;
    std::map<std::string, SparklineItem*> _sparkline_widgets;
    std::map<std::string, BatteryTelltaleWidget*> _battery_telltale_widgets;
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

