#include "main_window.h"

#include <spdlog/spdlog.h>
#include <QDebug>


MainWindow::MainWindow(const app_config_t& app_cfg, const window_config_t& window_cfg, bool libusb_debug):
    QWidget{},
    _app_cfg{app_cfg},
    _window_cfg{window_cfg},
    _carplay_widget{nullptr}
{
    setWindowTitle(QString("Mercedes Dash - %1").arg(QString::fromStdString(_window_cfg.name)));
    setFixedSize(_window_cfg.width, _window_cfg.height);

    // Create widgets from configuration
    createWidgetsFromConfig();
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
        return new SpeedometerWidgetMPH();
    }
    else if (type == "tachometer") {
        return new TachometerWidget();
    }
    else if (type == "sparkline") {
        // Sparkline requires units parameter - default to empty string
        return new SparklineItem("");
    }
    else if (type == "battery_telltale") {
        return new BatteryTelltaleWidget();
    }
    else if (type == "carplay") {
        // CarPlay widget needs special handling due to its constructor parameters
        auto* carplay = new CarPlayWidget(_app_cfg, false); // Using false for libusb_debug
        carplay->setSize(widget_config.width, widget_config.height);
        _carplay_widget = carplay; // Store reference for external access
        return carplay;
    }
    else {
        spdlog::warn("Unknown widget type: '{}'", type);
        return nullptr;
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