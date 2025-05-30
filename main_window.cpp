#include "main_window.h"

#include <spdlog/spdlog.h>


MainWindow::MainWindow(const app_config_t& app_cfg):
    QWidget{},
    _app_cfg{app_cfg},
    _carplay_widget{},
    _horizontal_layout{this}
{
    setWindowTitle("Mercedes Dash");
    setFixedSize(_app_cfg.width_px, _app_cfg.height_px);  // TODO: Make this configurable.

    _horizontal_layout.setContentsMargins(0, 0, 0, 0);
    _horizontal_layout.setSpacing(0);

    _carplay_widget.setSize(_app_cfg.width_px, _app_cfg.height_px);
    _horizontal_layout.addWidget(&_carplay_widget);


    QObject::connect(&_carplay_widget,  &CarPlayWidget::touchEvent, [this](TouchAction action, uint32_t x, uint32_t y)
    {
        emit
        (
            carplay_touch_event(action, x, y)
        );
    });
}

void MainWindow::update_carplay_image(const QPixmap& pixmap)
{
    _carplay_widget.setPixmap(pixmap);
}

#include "moc_main_window.cpp"