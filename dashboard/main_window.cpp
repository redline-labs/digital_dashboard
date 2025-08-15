#include "main_window.h"

#include <spdlog/spdlog.h>
#include <QDebug>
#include <QMetaObject>
#include <QPalette>

#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"

MainWindow::MainWindow(const window_config_t& window_cfg):
    QWidget{},
    _window_cfg{window_cfg}
{
    setWindowTitle(QString("Redline Dash - %1").arg(QString::fromStdString(_window_cfg.name)));
    setFixedSize(_window_cfg.width, _window_cfg.height);

    // Set background color from configuration
    setStyleSheet(QString("MainWindow { background-color: %1; }")
                 .arg(QString::fromStdString(_window_cfg.background_color)));

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
    for (const auto& widget_config : _window_cfg.widgets)
    {
        QWidget* widget = createWidget(widget_config);
        if (widget)
        {
            // Set parent and position
            widget->setParent(this);
            widget->setGeometry(widget_config.x, widget_config.y, widget_config.width, widget_config.height);
            widget->show();
            
            // Store the widget
            _widgets.emplace_back(std::unique_ptr<QWidget>(widget));
            
            SPDLOG_INFO("Created widget '{}' at ({}, {}) with size {}x{} in window '{}'",
                widget_type_to_string(widget_config.type),
                widget_config.x,
                widget_config.y, 
                widget_config.width,
                widget_config.height,
                _window_cfg.name);
        }
        else
        {
            SPDLOG_ERROR("Failed to create widget of type '{}' in window '{}'", 
                widget_type_to_string(widget_config.type),
                _window_cfg.name);
        }
    }
}

QWidget* MainWindow::createWidget(const widget_config_t& widget_config)
{    
    if (widget_config.type == widget_type_t::mercedes_190e_speedometer)
    {
        auto* speedometer = new Mercedes190ESpeedometer(std::get<Mercedes190ESpeedometerConfig_t>(widget_config.config));
        return speedometer;
    }
    else if (widget_config.type == widget_type_t::mercedes_190e_tachometer)
    {
        auto* tachometer = new Mercedes190ETachometer(std::get<Mercedes190ETachometerConfig_t>(widget_config.config));
        return tachometer;
    }
    else if (widget_config.type == widget_type_t::sparkline)
    {
        auto* sparkline = new SparklineItem(std::get<SparklineConfig_t>(widget_config.config));
        return sparkline;
    }
    else if (widget_config.type == widget_type_t::mercedes_190e_telltale)
    {
        auto* telltale = new Mercedes190ETelltale(std::get<Mercedes190ETelltaleConfig_t>(widget_config.config));
        return telltale;
    }
    else if (widget_config.type == widget_type_t::carplay)
    {
        // CarPlay widget needs special handling due to its constructor parameters
        auto* carplay = new CarPlayWidget(std::get<CarplayConfig_t>(widget_config.config));
        carplay->setSize(widget_config.width, widget_config.height);
        return carplay;
    }
    else if (widget_config.type == widget_type_t::mercedes_190e_cluster_gauge)
    {
        auto* cluster_gauge = new Mercedes190EClusterGauge(std::get<Mercedes190EClusterGaugeConfig_t>(widget_config.config));
        return cluster_gauge;
    }
    else if (widget_config.type == widget_type_t::motec_c125_tachometer)
    {
        auto* circle_tach = new MotecC125Tachometer(std::get<MotecC125TachometerConfig_t>(widget_config.config));
        return circle_tach;
    }
    else if (widget_config.type == widget_type_t::motec_cdl3_tachometer)
    {
        auto* tach = new MotecCdl3Tachometer(std::get<MotecCdl3TachometerConfig_t>(widget_config.config));
        return tach;
    }
    else if (widget_config.type == widget_type_t::static_text)
    {
        auto* txt = new StaticTextWidget(std::get<StaticTextConfig_t>(widget_config.config));
        return txt;
    }
    else
    {
        SPDLOG_WARN("Unknown widget type: '{}'", widget_type_to_string(widget_config.type));
        return nullptr;
    }
}


const std::string& MainWindow::getWindowName() const
{
    return _window_cfg.name;
}

#include "moc_main_window.cpp"