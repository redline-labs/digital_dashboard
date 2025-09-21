#include "editor/selection_frame.h"
#include "editor/widget_registry.h"

#include <QPainter>
#include <QEvent>

namespace
{
    // Colors for selection chrome
    // kSelectedOutlineColor: Active selection outline color (medium blue)
    // kUnselectedOutlineColor: Non-selected frame outline color in editor mode (dark gray)
    constexpr QColor kSelectedOutlineColor(0, 122, 255);
    constexpr QColor kUnselectedOutlineColor(80, 80, 80);
}

SelectionFrame::SelectionFrame(widget_type_t type, QWidget* parent)
    : QWidget(parent), type_(type), child_(nullptr)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground);
    QWidget* w = widget_registry::instantiateWidget(type_, nullptr);
    setChild(w);
    // Overlay that always draws above child
    overlay_ = new QWidget(this);
    overlay_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlay_->setAttribute(Qt::WA_NoSystemBackground, true);
    overlay_->resize(size());
    overlay_->raise();
    overlay_->installEventFilter(this);
}

void SelectionFrame::setChild(QWidget* newChild)
{
    if (child_ == newChild)
    {
        return;
    }

    if (child_)
    {
        child_->setParent(nullptr);
        child_->deleteLater();
    }

    child_ = newChild;

    if (child_)
    {
        child_->setParent(this);
        child_->move(0, 0);
        // Keep frame size; resize new child to fit current frame
        child_->resize(size());
        child_->show();
    }
}

void SelectionFrame::setSelected(bool on)
{
    if (selected_ == on)
    {
        return;
    }

    selected_ = on;
    update();
}

void SelectionFrame::setEditorModeCapture(bool on)
{
    editorMode_ = on;
    if (child_)
    {
        child_->setAttribute(Qt::WA_TransparentForMouseEvents, on);
    }

    update();
}

SelectionFrame::Handle SelectionFrame::hitTestCanvasPos(const QPoint& canvasPos) const
{
    const QPoint pos = mapFromParent(canvasPos);
    const QRect r(0, 0, width(), height());
    const QRect tl(r.topLeft() - QPoint(kGrabHandleSizePx/2, kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    const QRect tr(QPoint(r.right() - kGrabHandleSizePx/2, r.top() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    const QRect bl(QPoint(r.left() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    const QRect br(QPoint(r.right() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    if (tl.contains(pos)) return Handle::ResizeTL;
    if (tr.contains(pos)) return Handle::ResizeTR;
    if (bl.contains(pos)) return Handle::ResizeBL;
    if (br.contains(pos)) return Handle::ResizeBR;
    if (r.contains(pos)) return Handle::Move;
    return Handle::None;
}

void SelectionFrame::paintEvent(QPaintEvent* /*event*/)
{
    // no selection chrome in non-editor mode
    if (!editorMode_)
    {
        return;
    }
    if (overlay_)
    {
        overlay_->raise();
    }
}

void SelectionFrame::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (child_)
    {
        child_->resize(size());
    }
    if (overlay_)
    {
        overlay_->resize(size());
    }
}

bool SelectionFrame::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == overlay_ && event->type() == QEvent::Paint)
    {
        if (!editorMode_) return false;
        QPainter p(static_cast<QWidget*>(obj));
        p.setRenderHint(QPainter::Antialiasing);
        const bool drawHandles = selected_;
        const QColor outline = selected_ ? kSelectedOutlineColor : kUnselectedOutlineColor;
        QPen pen(outline);
        pen.setWidth(2);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRect outer = static_cast<QWidget*>(obj)->rect().adjusted(0, 0, -1, -1);
        p.drawRect(outer);
        if (drawHandles)
        {
            const QRect r = outer;
            const QRect handles[] = {
                QRect(r.topLeft() - QPoint(kGrabHandleSizePx/2, kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
                QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.top() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
                QRect(QPoint(r.left() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
                QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx))
            };
            p.setBrush(outline);
            for (const auto& h : handles) p.drawRect(h);
        }
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

#include "editor/moc_selection_frame.cpp"


