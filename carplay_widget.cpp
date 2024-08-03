#include "carplay_widget.h"
#include "touch_action.h"

#include <spdlog/spdlog.h>

CarPlayWidget::CarPlayWidget() :
    QLabel()
{
}

CarPlayWidget::~CarPlayWidget()
{
}

void CarPlayWidget::setSize(uint32_t width_px, uint32_t height_px)
{
    setFixedSize({static_cast<int>(width_px), static_cast<int>(height_px)});
}

void CarPlayWidget::mousePressEvent(QMouseEvent* e)
{
    emit (
        touchEvent(TouchAction::Down, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()))
    );
}

void CarPlayWidget::mouseReleaseEvent(QMouseEvent* e)
{
    emit (
        touchEvent(TouchAction::Up, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()))
    );
}

#include "moc_carplay_widget.cpp"