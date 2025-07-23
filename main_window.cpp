#include "main_window.h"

#include <spdlog/spdlog.h>
#include <QDebug>
#include <QMetaObject>
#include <QPalette>


MainWindow::MainWindow(const window_config_t& window_cfg):
    QWidget{},
    _window_cfg{window_cfg}
{
    setWindowTitle(QString("Mercedes Dash - %1").arg(QString::fromStdString(_window_cfg.name)));
    setFixedSize(_window_cfg.width, _window_cfg.height);

    // Set background color from configuration
    setStyleSheet(QString("MainWindow { background-color: %1; }")
                 .arg(QString::fromStdString(_window_cfg.background_color)));

    // Initialize Zenoh first
    initializeZenoh();

    // Create widgets from configuration
    createWidgetsFromConfig();
}

MainWindow::~MainWindow()
{
    // Zenoh session will be automatically cleaned up by shared_ptr
    // Individual widgets will clean up their own subscriptions
}

void MainWindow::createWidgetsFromConfig()
{
    for (const auto& widget_config : _window_cfg.widgets) {
        QWidget* widget = createWidget(widget_config);
        if (widget) {
            // Set parent and position
            widget->setParent(this);
            widget->setGeometry(widget_config.x, widget_config.y, widget_config.width, widget_config.height);
            widget->show();
            
            // Store the widget
            _widgets.emplace_back(std::unique_ptr<QWidget>(widget));
            
            spdlog::info("Created widget '{}' at ({}, {}) with size {}x{} in window '{}'",
                        widget_config.type, widget_config.x, widget_config.y, 
                        widget_config.width, widget_config.height, _window_cfg.name);
        } else {
            spdlog::error("Failed to create widget of type '{}' in window '{}'", 
                         widget_config.type, _window_cfg.name);
        }
    }
}

QWidget* MainWindow::createWidget(const widget_config_t& widget_config)
{
    const std::string& type = widget_config.type;
    
    if (type == "speedometer") {
        auto* speedometer = new Mercedes190ESpeedometer(std::get<Mercedes190ESpeedometerConfig_t>(widget_config.config));
        if (_zenoh_session) {
            speedometer->setZenohSession(_zenoh_session);
        }
        return speedometer;
    }
    else if (type == "tachometer") {
        auto* tachometer = new Mercedes190ETachometer(std::get<Mercedes190ETachometerConfig_t>(widget_config.config));
        if (_zenoh_session) {
            tachometer->setZenohSession(_zenoh_session);
        }
        return tachometer;
    }
    else if (type == "sparkline") {
        auto* sparkline = new SparklineItem(std::get<SparklineConfig_t>(widget_config.config));
        if (_zenoh_session) {
            sparkline->setZenohSession(_zenoh_session);
        }
        return sparkline;
    }
    else if (type == "battery_telltale") {
        auto* battery_telltale = new Mercedes190EBatteryTelltale(std::get<Mercedes190EBatteryTelltaleConfig_t>(widget_config.config));
        if (_zenoh_session) {
            battery_telltale->setZenohSession(_zenoh_session);
        }
        return battery_telltale;
    }
    else if (type == "carplay") {
        // CarPlay widget needs special handling due to its constructor parameters
        auto* carplay = new CarPlayWidget(std::get<CarplayConfig_t>(widget_config.config));
        carplay->setSize(widget_config.width, widget_config.height);
        return carplay;
    }
    else if (type == "cluster_gauge") {
        auto* cluster_gauge = new Mercedes190EClusterGauge({});
        return cluster_gauge;
    }
    else {
        spdlog::warn("Unknown widget type: '{}'", type);
        return nullptr;
    }
}

void MainWindow::initializeZenoh()
{
    try {
        // Create Zenoh configuration
        auto config = zenoh::Config::create_default();
        
        // Open Zenoh session (use shared_ptr for sharing with widgets)
        _zenoh_session = std::make_shared<zenoh::Session>(zenoh::Session::open(std::move(config)));
        spdlog::info("Zenoh session opened successfully for window '{}'", _window_cfg.name);
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize Zenoh for window '{}': {}", _window_cfg.name, e.what());
        // Continue without Zenoh - widgets can still function without real-time data
    }
}



const std::string& MainWindow::getWindowName() const
{
    return _window_cfg.name;
}

#include "moc_main_window.cpp"