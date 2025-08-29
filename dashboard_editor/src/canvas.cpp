#include "dashboard_editor/canvas.h"
#include "dashboard_editor/editor_constants.h"
#include "dashboard_editor/widget_registry.h"
#include "dashboard_editor/selection_overlay.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QLabel>
#include <QApplication>
#include <QKeyEvent>

namespace {
    constexpr int kGridStepPx = 20;
    constexpr QColor kGridColor = QColor(60,60,60);
}

Canvas::Canvas(QWidget* parent) :
  QWidget(parent),
  interceptInteractions_(true)
{
    setAcceptDrops(true);
    setAutoFillBackground(true);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(30,30,30));
    setPalette(pal);
    // Default size
    resize(editor_defaults::kDefaultCanvasWidth, editor_defaults::kDefaultCanvasHeight);
    setFocusPolicy(Qt::StrongFocus);

    // Overlay draws selection/handles above child widgets
    overlay_ = new SelectionOverlay(this);
    overlay_->resize(size());
    overlay_->raise();
}

void Canvas::setInterceptInteractions(bool intercept)
{
    interceptInteractions_ = intercept;
    // Toggle mouse transparency on all child widgets recursively
    const auto children = findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* c : children)
    {
        setMouseTransparentRecursive(c, interceptInteractions_);
    }
}

void Canvas::setBackgroundColor(const QString& hexColor)
{
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(hexColor));
    setPalette(pal);
    update();
}

void Canvas::setMouseTransparentRecursive(QWidget* w, bool on)
{
    if (!w)
    {
        return;
    }

    w->setAttribute(Qt::WA_TransparentForMouseEvents, on);
    const auto children = w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* c : children)
    {
        setMouseTransparentRecursive(c, on);
    }
}

void Canvas::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasText())
    {
        event->acceptProposedAction();
    }
}

void Canvas::dropEvent(QDropEvent* event)
{
    const QString typeKey = event->mimeData()->text();
    const widget_type_t type = reflection::enum_traits<widget_type_t>::from_string(typeKey.toStdString());
    QWidget* w = widget_registry::instantiateWidget(type, this);
    if (w)
    {
        w->setParent(this);
        const QPoint pos = event->position().toPoint();
        // Provide a reasonable default size for various widgets
        if (w->sizeHint().isValid())
        {
            w->resize(w->sizeHint());
        }
        else
        {
            w->resize(200, 200);
        }

        w->move(pos);
        w->show();
        // Apply current intercept mode to the new widget subtree
        setMouseTransparentRecursive(w, interceptInteractions_);
        Item item;
        item.widget = w;
        item.position = pos;
        items_.push_back(std::move(item));
        update();
        event->acceptProposedAction();

        // Select newly dropped widget
        selected_ = w;
        selectedRect_ = widgetRect(selected_);
        dragMode_ = DragMode::None;
        emit selectionChanged(selected_);
    }
}

void Canvas::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kGridColor));

    // Draw a simple grid
    for (int x = 0; x < width(); x += kGridStepPx)
    {
        p.drawLine(x, 0, x, height());
    }

    for (int y = 0; y < height(); y += kGridStepPx)
    {
        p.drawLine(0, y, width(), y);
    }
}

void Canvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (overlay_) overlay_->resize(size());
}

QRect Canvas::widgetRect(QWidget* w) const
{
    return QRect(w->pos(), w->size());
}

QWidget* Canvas::topLevelWidgetAt(const QPoint& pos) const
{
    // Iterate items_ in reverse so the most recently added (topmost) gets priority
    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    {
        QWidget* w = it->widget;
        if (!w || w->isHidden()) continue;
        if (widgetRect(w).contains(pos)) return w;
    }
    return nullptr;
}

Canvas::DragMode Canvas::hitTestHandles(const QRect& r, const QPoint& pos) const
{
    const QRect tl(r.topLeft() - QPoint(SelectionOverlay::kGrabHandleSizePx/2, SelectionOverlay::kGrabHandleSizePx/2), QSize(SelectionOverlay::kGrabHandleSizePx, SelectionOverlay::kGrabHandleSizePx));
    const QRect tr(QPoint(r.right() - SelectionOverlay::kGrabHandleSizePx/2, r.top() - SelectionOverlay::kGrabHandleSizePx/2), QSize(SelectionOverlay::kGrabHandleSizePx, SelectionOverlay::kGrabHandleSizePx));
    const QRect bl(QPoint(r.left() - SelectionOverlay::kGrabHandleSizePx/2, r.bottom() - SelectionOverlay::kGrabHandleSizePx/2), QSize(SelectionOverlay::kGrabHandleSizePx, SelectionOverlay::kGrabHandleSizePx));
    const QRect br(QPoint(r.right() - SelectionOverlay::kGrabHandleSizePx/2, r.bottom() - SelectionOverlay::kGrabHandleSizePx/2), QSize(SelectionOverlay::kGrabHandleSizePx, SelectionOverlay::kGrabHandleSizePx));

    if (tl.contains(pos)) return DragMode::ResizeTL;
    if (tr.contains(pos)) return DragMode::ResizeTR;
    if (bl.contains(pos)) return DragMode::ResizeBL;
    if (br.contains(pos)) return DragMode::ResizeBR;
    if (r.contains(pos)) return DragMode::Move;

    return DragMode::None;
}

void Canvas::mousePressEvent(QMouseEvent* event)
{
    if (!interceptInteractions_)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint pos = event->pos();
    // Determine if clicking on a top-level child widget using stored layout (works with transparent children)
    QWidget* topLevel = topLevelWidgetAt(pos);
    if (topLevel)
    {
        selected_ = topLevel;
        selectedRect_ = widgetRect(selected_);
        dragMode_ = hitTestHandles(selectedRect_, pos);
        dragStartPos_ = pos;
        dragStartRect_ = selectedRect_;
        if (overlay_) overlay_->setSelectionRect(selectedRect_);
        update();
        emit selectionChanged(selected_);
        return;
    }

    // Not clicking inside any widget's rect. If we already have a selection, allow grabs on handles even if they extend outside the rect
    if (selected_)
    {
        DragMode hm = hitTestHandles(selectedRect_, pos);
        if (hm != DragMode::None)
        {
            dragMode_ = hm;
            dragStartPos_ = pos;
            dragStartRect_ = selectedRect_;
            if (overlay_) overlay_->setSelectionRect(selectedRect_);
            update();
            return;
        }
    }
    // Otherwise clear selection
    selected_ = nullptr;
    dragMode_ = DragMode::None;
    if (overlay_) overlay_->setSelectionRect(QRect());
    update();
    emit selectionChanged(nullptr);
}

void Canvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!interceptInteractions_)
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (!selected_) { return; }
    const QPoint delta = event->pos() - dragStartPos_;
    QRect r = dragStartRect_;
    switch (dragMode_) {
    case DragMode::Move:
        r.moveTopLeft(r.topLeft() + delta);
        break;
    case DragMode::ResizeTL:
        r.setTopLeft(r.topLeft() + delta);
        break;
    case DragMode::ResizeTR:
        r.setTopRight(r.topRight() + delta);
        break;
    case DragMode::ResizeBL:
        r.setBottomLeft(r.bottomLeft() + delta);
        break;
    case DragMode::ResizeBR:
        r.setBottomRight(r.bottomRight() + delta);
        break;
    case DragMode::None:
        return;
    }
    // Constrain minimal size
    constexpr int minW = 20;
    constexpr int minH = 20;
    if (r.width() < minW) r.setWidth(minW);
    if (r.height() < minH) r.setHeight(minH);

    selected_->move(r.topLeft());
    selected_->resize(r.size());
    if (overlay_) overlay_->setSelectionRect(widgetRect(selected_));
    update();
    
}

void Canvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (!interceptInteractions_)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    dragMode_ = DragMode::None;
}

void Canvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
    {
        if (selected_)
        {
            auto it = std::remove_if(items_.begin(), items_.end(), [this](const Item& item){ return item.widget == selected_; });
            items_.erase(it, items_.end());
            selected_->deleteLater();
            selected_ = nullptr;
            update();
            emit selectionChanged(nullptr);
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

QWidget* Canvas::createWidgetForType(const QString& typeKey, QWidget* parent)
{
    const widget_type_t type = reflection::enum_traits<widget_type_t>::from_string(typeKey.toStdString());
    return widget_registry::instantiateWidget(type, parent);
}


void Canvas::replaceWidget(QWidget* oldWidget, QWidget* newWidget, const QRect& rect)
{
    if (!oldWidget || !newWidget) return;
    // Adopt under canvas
    newWidget->setParent(this);
    newWidget->setGeometry(rect);
    newWidget->show();
    setMouseTransparentRecursive(newWidget, interceptInteractions_);

    // Swap in items_ list
    for (auto& it : items_)
    {
        if (it.widget == oldWidget)
        {
            it.widget = newWidget;
            it.position = rect.topLeft();
            break;
        }
    }
    // Remove old widget
    if (selected_ == oldWidget) selected_ = newWidget;
    oldWidget->deleteLater();
    if (overlay_) overlay_->setSelectionRect(widgetRect(newWidget));
    update();
    emit selectionChanged(newWidget);
}

#include "dashboard_editor/moc_canvas.cpp"
