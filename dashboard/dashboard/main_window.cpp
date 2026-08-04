#include "dashboard/main_window.h"

#include <spdlog/spdlog.h>
#include <QDebug>
#include <QMetaObject>
#include <QPalette>

#include "reflection/reflection.h"

#include "dashboard/widget_factory.h"
#include "dashboard/widget_identity.h"

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
    std::size_t index = 0;
    for (const auto& widget_config : _app_cfg.widgets)
    {
        const std::size_t this_index = index++;

        QWidget* widget = widget_factory::createWidgetFromConfig(widget_config, this);
        if (widget)
        {
            // Name it before anything else can look at it: this is what the
            // agent control interface addresses widgets by.
            dashboard::applyWidgetIdentity(widget, widget_config, this_index);

            // Set position
            widget->setGeometry(widget_config.x, widget_config.y, widget_config.width, widget_config.height);
            widget->show();

            // Store the widget alongside the config entry it came from, so a
            // later rebuild updates the right one.
            _widgets.emplace_back(LiveWidget{std::unique_ptr<QWidget>(widget), this_index});

            SPDLOG_INFO("Created widget '{}' (id '{}') at ({}, {}) with size {}x{} in window '{}'",
                reflection::enum_to_string(widget_config.type),
                widget->objectName().toStdString(),
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

bool MainWindow::rebuildWidget(QWidget* existing, const widget_config_t& cfg)
{
    for (std::size_t i = 0; i < _widgets.size(); ++i)
    {
        if (_widgets[i].widget.get() != existing)
        {
            continue;
        }

        // Keep the identity and placement the caller already knows about --
        // a set_config that silently moved or renamed the widget would
        // invalidate every selector and ref pointing at it.
        const QRect geometry = existing->geometry();
        const QString object_name = existing->objectName();

        QWidget* replacement = widget_factory::createWidgetFromConfig(cfg, this);
        if (replacement == nullptr)
        {
            SPDLOG_ERROR("Rebuilding widget '{}' failed; leaving the original in place.",
                         object_name.toStdString());
            return false;
        }

        replacement->setObjectName(object_name);
        replacement->setGeometry(geometry);
        replacement->show();

        // Keep the stored config in step, so a later read reports what is
        // actually on screen. Index through the widget's own config_index: the
        // position in _widgets is not the position in _app_cfg.widgets once any
        // widget has failed to build.
        const std::size_t cfg_index = _widgets[i].config_index;
        if (cfg_index < _app_cfg.widgets.size())
        {
            const std::string id = _app_cfg.widgets[cfg_index].id;
            _app_cfg.widgets[cfg_index] = cfg;
            _app_cfg.widgets[cfg_index].id = id;
            _app_cfg.widgets[cfg_index].x = static_cast<int16_t>(geometry.x());
            _app_cfg.widgets[cfg_index].y = static_cast<int16_t>(geometry.y());
            _app_cfg.widgets[cfg_index].width = static_cast<uint16_t>(geometry.width());
            _app_cfg.widgets[cfg_index].height = static_cast<uint16_t>(geometry.height());
        }

        // Assigning the unique_ptr destroys the old widget immediately. That is
        // wanted here: the caller is about to snapshot, and a deleteLater'd
        // widget would still be in the tree when it did.
        _widgets[i].widget = std::unique_ptr<QWidget>(replacement);
        return true;
    }

    return false;
}

#include "dashboard/moc_main_window.cpp"