#include "main_window.h"

#include <spdlog/spdlog.h>
#include <QDebug>
#include <QMetaObject>
#include <QPalette>


MainWindow::MainWindow(const app_config_t& app_cfg, const window_config_t& window_cfg):
    QWidget{},
    _app_cfg{app_cfg},
    _window_cfg{window_cfg},
    _carplay_widget{nullptr}
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
    // Zenoh session and subscribers will be automatically cleaned up by unique_ptr
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
            
            // Create Zenoh subscription if specified
            if (!widget_config.zenoh_key.empty() && _zenoh_session) {
                createZenohSubscription(widget_config.zenoh_key, widget_config.type);
            }
            
            spdlog::info("Created widget '{}' at ({}, {}) with size {}x{} in window '{}'{}",
                        widget_config.type, widget_config.x, widget_config.y, 
                        widget_config.width, widget_config.height, _window_cfg.name,
                        widget_config.zenoh_key.empty() ? "" : " with Zenoh key: " + widget_config.zenoh_key);
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
        auto* speedometer = new SpeedometerWidgetMPH(std::get<speedometer_config_t>(widget_config.config));
        // Store mapping for Zenoh updates if key is provided
        if (!widget_config.zenoh_key.empty()) {
            _speedometer_widgets[widget_config.zenoh_key] = speedometer;
        }
        return speedometer;
    }
    else if (type == "tachometer") {
        auto* tachometer = new TachometerWidget(std::get<tachometer_config_t>(widget_config.config));
        // Store mapping for Zenoh updates if key is provided
        if (!widget_config.zenoh_key.empty()) {
            _tachometer_widgets[widget_config.zenoh_key] = tachometer;
        }
        return tachometer;
    }
    else if (type == "sparkline") {
        // Sparkline requires units parameter - default to empty string
        auto* sparkline = new SparklineItem(std::get<sparkline_config_t>(widget_config.config));
        // Store mapping for Zenoh updates if key is provided
        if (!widget_config.zenoh_key.empty()) {
            _sparkline_widgets[widget_config.zenoh_key] = sparkline;
        }
        return sparkline;
    }
    else if (type == "battery_telltale") {
        auto* battery_telltale = new BatteryTelltaleWidget(std::get<battery_telltale_config_t>(widget_config.config));
        // Store mapping for Zenoh updates if key is provided
        if (!widget_config.zenoh_key.empty()) {
            _battery_telltale_widgets[widget_config.zenoh_key] = battery_telltale;
        }
        return battery_telltale;
    }
    else if (type == "carplay") {
        // CarPlay widget needs special handling due to its constructor parameters
        auto* carplay = new CarPlayWidget(std::get<carplay_config_t>(widget_config.config));
        carplay->setSize(widget_config.width, widget_config.height);
        _carplay_widget = carplay; // Store reference for external access
        return carplay;
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
        
        // Open Zenoh session
        _zenoh_session = std::make_unique<zenoh::Session>(zenoh::Session::open(std::move(config)));
        spdlog::info("Zenoh session opened successfully for window '{}'", _window_cfg.name);
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize Zenoh for window '{}': {}", _window_cfg.name, e.what());
        // Continue without Zenoh - widgets can still function without real-time data
    }
}

void MainWindow::createZenohSubscription(const std::string& zenoh_key, const std::string& widget_type)
{
    if (!_zenoh_session) {
        spdlog::warn("Cannot create Zenoh subscription for key '{}' - session not available", zenoh_key);
        return;
    }

    try {
        auto key_expr = zenoh::KeyExpr(zenoh_key);
        
        auto subscriber = std::make_unique<zenoh::Subscriber<void>>(
            _zenoh_session->declare_subscriber(
                key_expr,
                [this, zenoh_key, widget_type](const zenoh::Sample& sample) {
                    try {
                        // Convert payload to string and then to appropriate data type
                        const auto& payload = sample.get_payload();
                        std::string data_str = payload.as_string();
                        
                        if (widget_type == "speedometer") {
                            double speed_mps = std::stod(data_str);
                            // Call slot via Qt's queued connection to ensure thread safety
                            QMetaObject::invokeMethod(this, "onSpeedDataReceived", 
                                                    Qt::QueuedConnection, 
                                                    Q_ARG(double, speed_mps));
                        }
                        else if (widget_type == "tachometer") {
                            double rpm = std::stod(data_str);
                            QMetaObject::invokeMethod(this, "onRpmDataReceived", 
                                                    Qt::QueuedConnection, 
                                                    Q_ARG(double, rpm));
                        }
                        else if (widget_type == "sparkline") {
                            double value = std::stod(data_str);
                            QMetaObject::invokeMethod(this, "onSparklineDataReceived", 
                                                    Qt::QueuedConnection, 
                                                    Q_ARG(double, value));
                        }
                        else if (widget_type == "battery_telltale") {
                            bool asserted = (data_str == "true" || data_str == "1");
                            QMetaObject::invokeMethod(this, "onBatteryTelltaleDataReceived", 
                                                    Qt::QueuedConnection, 
                                                    Q_ARG(bool, asserted));
                        }

                    } catch (const std::exception& e) {
                        spdlog::error("Error processing {} data from key '{}': {}", widget_type, zenoh_key, e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        
        _zenoh_subscribers.push_back(std::move(subscriber));
        spdlog::info("Created Zenoh subscription for key '{}' (widget type: {})", zenoh_key, widget_type);
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to create Zenoh subscription for key '{}': {}", zenoh_key, e.what());
    }
}

void MainWindow::onSpeedDataReceived(double speedMps)
{
    // Convert m/s to mph (1 m/s = 2.23694 mph)
    double speedMph = speedMps * 2.23694;
    
    // Update all speedometer widgets that have active subscriptions
    for (auto& [key, widget] : _speedometer_widgets) {
        if (widget) {
            widget->setSpeed(static_cast<float>(speedMph));
        }
    }
}

void MainWindow::onRpmDataReceived(double rpm)
{
    // Update all tachometer widgets that have active subscriptions
    for (auto& [key, widget] : _tachometer_widgets) {
        if (widget) {
            widget->setRpm(static_cast<float>(rpm));
        }
    }
}

void MainWindow::onSparklineDataReceived(double value)
{
    // Update all sparkline widgets that have active subscriptions
    for (auto& [key, widget] : _sparkline_widgets) {
        if (widget) {
            widget->addDataPoint(value);
        }
    }
}

void MainWindow::onBatteryTelltaleDataReceived(bool asserted)
{
    // Update all battery telltale widgets that have active subscriptions
    for (auto& [key, widget] : _battery_telltale_widgets) {
        if (widget) {
            widget->setAsserted(asserted);
        }
    }
}

CarPlayWidget* MainWindow::getCarPlayWidget()
{
    return _carplay_widget;
}

const std::string& MainWindow::getWindowName() const
{
    return _window_cfg.name;
}

#include "moc_main_window.cpp"