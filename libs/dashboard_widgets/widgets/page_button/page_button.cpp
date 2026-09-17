#include "page_button/page_button.h"

#include "dashboard/page_command_publisher.h"
#include "qt_helpers/widget_colors.h"

#include <QColor>
#include <QFont>
#include <QMouseEvent>
#include <QPainter>

PageButtonWidget::PageButtonWidget(PageButtonConfig_t cfg, QWidget* parent)
    : QWidget(parent)
    , _cfg(std::move(cfg))
    , _sender(std::make_unique<dashboard::PageCommandSender>())
{
}

PageButtonWidget::~PageButtonWidget() = default;

void PageButtonWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QColor fill = qt_helpers::toQColor(_pressed ? _cfg.pressed_color : _cfg.background_color);
    painter.setBrush(fill);
    const qreal radius = _cfg.corner_radius;
    painter.drawRoundedRect(QRectF(rect()), radius, radius);

    QFont font = painter.font();
    font.setPointSize(_cfg.font_size);
    painter.setFont(font);
    painter.setPen(qt_helpers::toQColor(_cfg.text_color, Qt::white));
    painter.drawText(rect(), Qt::AlignCenter, QString::fromStdString(_cfg.label));
}

void PageButtonWidget::mousePressEvent(QMouseEvent* event)
{
    _pressed = true;
    update();
    event->accept();
}

void PageButtonWidget::mouseReleaseEvent(QMouseEvent* event)
{
    // A finger that slides off before lifting has changed its mind.
    const bool inside = rect().contains(event->position().toPoint());
    const bool was_pressed = _pressed;
    _pressed = false;
    update();
    event->accept();
    if (was_pressed && inside)
    {
        _sender->send(_cfg.command);
    }
}

#include "page_button/moc_page_button.cpp"
