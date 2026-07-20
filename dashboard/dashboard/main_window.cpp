#include "dashboard/main_window.h"

#include <spdlog/spdlog.h>
#include <QDebug>
#include <QMetaObject>
#include <QPalette>

#include "reflection/reflection.h"

#include "dashboard/widget_factory.h"

MainWindow::MainWindow(const app_config_t& app_cfg):
    QWidget{},
    _app_cfg{app_cfg}
{
    setWindowTitle(QString("Redline Dash - %1").arg(QString::fromStdString(_app_cfg.name)));
    setFixedSize(_app_cfg.width, _app_cfg.height);

    // Set background color from configuration
    setStyleSheet(QString("MainWindow { background-color: %1; }")
                 .arg(QString::fromStdString(_app_cfg.background_color)));

    // Create widgets from configuration
    createWidgetsFromConfig();
}

MainWindow::~MainWindow() = default;

void MainWindow::createWidgetsFromConfig()
{
    for (const auto& widget_config : _app_cfg.widgets)
    {
        QWidget* widget = widget_factory::createWidgetFromConfig(widget_config, this);
        if (widget)
        {
            // Set position
            widget->setGeometry(widget_config.x, widget_config.y, widget_config.width, widget_config.height);
            widget->show();
            
            // Store the widget
            _widgets.emplace_back(std::unique_ptr<QWidget>(widget));
            
            SPDLOG_INFO("Created widget '{}' at ({}, {}) with size {}x{} in window '{}'",
                reflection::enum_to_string(widget_config.type),
                widget_config.x,
                widget_config.y, 
                widget_config.width,
                widget_config.height,
                _app_cfg.name);
        }
        else
        {
            SPDLOG_ERROR("Failed to create widget of type '{}' in window '{}'", 
                reflection::enum_to_string(widget_config.type),
                _app_cfg.name);
        }
    }
}

const std::string& MainWindow::getWindowName() const
{
    return _app_cfg.name;
}

#include "dashboard/moc_main_window.cpp"