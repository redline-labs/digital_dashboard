#include "dashboard_editor/selection_overlay.h"

#include <QPainter>

SelectionOverlay::SelectionOverlay(QWidget* parent)
    : QWidget(parent), rect_()
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_AlwaysStackOnTop);
}

void SelectionOverlay::setSelectionRect(const QRect& rect)
{
    rect_ = rect;
    update();
}

void SelectionOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    if (rect_.isNull()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(0, 122, 255));
    pen.setWidth(2);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect_);

    // Draw resize handles at corners
    
    const QRect r = rect_;
    const QRect handles[] = {
        QRect(r.topLeft() - QPoint(kGrabHandleSizePx/2, kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
        QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.top() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
        QRect(QPoint(r.left() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
        QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx))
    };
    p.setBrush(QColor(0,122,255));
    for (const auto& h : handles) p.drawRect(h);
}

#include "dashboard_editor/moc_selection_overlay.cpp"
