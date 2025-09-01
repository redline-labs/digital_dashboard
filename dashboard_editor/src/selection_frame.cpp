#include "dashboard_editor/selection_frame.h"

#include <QPainter>

SelectionFrame::SelectionFrame(widget_type_t type, QWidget* child, QWidget* parent)
    : QWidget(parent), type_(type), child_(child)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground);
    if (child_)
    {
        child_->setParent(this);
        child_->move(0, 0);
        child_->show();
        // size to child
        resize(child_->size());
    }
}

void SelectionFrame::setChild(QWidget* newChild)
{
    if (child_ == newChild) return;
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
    if (selected_ == on) return;
    selected_ = on;
    update();
}

void SelectionFrame::setEditorModeCapture(bool on)
{
    editorMode_ = on;
    if (child_) child_->setAttribute(Qt::WA_TransparentForMouseEvents, on);
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

void SelectionFrame::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    if (!editorMode_) return; // no selection chrome in non-editor mode

    // Colors for selection chrome
    // kSelectedOutlineColor: Active selection outline color (medium blue)
    // kUnselectedOutlineColor: Non-selected frame outline color in editor mode (dark gray)
    constexpr QColor kSelectedOutlineColor(0, 122, 255);
    constexpr QColor kUnselectedOutlineColor(80, 80, 80);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool drawHandles = selected_;
    const QColor outline = selected_ ? kSelectedOutlineColor : kUnselectedOutlineColor;
    QPen pen(outline);
    pen.setWidth(2);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    if (drawHandles)
    {
        const QRect r = rect().adjusted(0, 0, -1, -1);
        const QRect handles[] = {
            QRect(r.topLeft() - QPoint(kGrabHandleSizePx/2, kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
            QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.top() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
            QRect(QPoint(r.left() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
            QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx))
        };
        p.setBrush(outline);
        for (const auto& h : handles) p.drawRect(h);
    }
}

void SelectionFrame::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (child_) child_->resize(size());
}

#include "dashboard_editor/moc_selection_frame.cpp"


