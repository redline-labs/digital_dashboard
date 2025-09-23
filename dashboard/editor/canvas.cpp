#include "editor/canvas.h"
#include "editor/editor_constants.h"
#include "editor/selection_frame.h"
//#include "carplay/carplay_widget.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QLabel>
#include <QApplication>
#include <QKeyEvent>
#include <variant>

namespace {
    constexpr int kGridStepPx = 20;
    constexpr QColor kGridColor = QColor(60,60,60);
    constexpr QColor kDefaultBackgroundColor = QColor(0, 0, 0);
}

Canvas::Canvas(QWidget* parent) :
  QWidget(parent),
  editorMode_(true)
{
    setAcceptDrops(true);
    setAutoFillBackground(true);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    // Set the background color to the default.
    setBackgroundColor(kDefaultBackgroundColor.name());

    // Default size
    resize(editor_defaults::kDefaultCanvasWidth, editor_defaults::kDefaultCanvasHeight);
    setFocusPolicy(Qt::StrongFocus);

    // Using per-widget SelectionFrame; no global overlay
}

void Canvas::clearAll()
{
    // Deselect existing
    if (auto* prev = qobject_cast<SelectionFrame*>(selected_)) prev->setSelected(false);
    selected_ = nullptr;
    // Delete widgets
    for (auto& item : items_)
    {
        if (item.widget)
        {
            item.widget->deleteLater();
        }
    }
    items_.clear();
    update();
    emit selectionChanged(nullptr);
}

void Canvas::loadFromAppConfig(const app_config_t& app_cfg)
{
    // Remove existing first
    clearAll();

    // Canvas adopts window size and background color
    resize(app_cfg.width, app_cfg.height);
    setBackgroundColor(QString::fromStdString(app_cfg.background_color));

    // Create and place widgets per config
    for (const auto& wcfg : app_cfg.widgets)
    {
        if (wcfg.type == widget_type_t::unknown)
        {
            SPDLOG_WARN("Skipping widget with unknown type at ({}, {})", wcfg.x, wcfg.y);
            continue;
        }
        SelectionFrame* frame = new SelectionFrame(wcfg.type, this);
        if (!frame)
        {
            continue;
        }

        // Apply typed widget configuration
        std::visit([&](auto const& cfg){ frame->applyConfig(cfg); }, wcfg.config);

        // If child provides a size hint, prefer it, otherwise use config size
        QSize targetSize(wcfg.width, wcfg.height);

        frame->move(wcfg.x, wcfg.y);
        if (frame->child()) frame->child()->resize(targetSize);
        // Some widgets require explicit size propagation to internal content
        /*if (auto* cp = qobject_cast<CarPlayWidget*>(frame->child()))
        {
            cp->setSize(targetSize.width(), targetSize.height());
        }*/
        frame->resize(targetSize);
        frame->show();

        // Apply editor mode mouse transparency
        frame->setEditorModeCapture(editorMode_);

        Item item;
        item.widget = frame;
        item.type = wcfg.type;
        item.position = QPoint(wcfg.x, wcfg.y);
        items_.push_back(std::move(item));
    }

    // No selection after load
    selected_ = nullptr;
    dragMode_ = DragMode::None;
    update();
}

app_config_t Canvas::exportAppConfig(const std::string& window_name) const
{
    app_config_t cfg;
    cfg.name = window_name;
    cfg.width = static_cast<uint16_t>(width());
    cfg.height = static_cast<uint16_t>(height());
    cfg.background_color = getBackgroundColorHex().toStdString();

    for (const auto& item : items_)
    {
        if (!item.widget) continue;
        const QRect rect = widgetRect(item.widget);
        if (auto* frame = qobject_cast<SelectionFrame*>(item.widget))
        {
            cfg.widgets.push_back(frame->toWidgetConfig(rect));
        }
    }

    return cfg;
}

void Canvas::setEditorMode(bool enabled)
{
    editorMode_ = enabled;
    // Toggle mouse transparency on all child widgets recursively
    const auto children = findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* c : children)
    {
        setMouseTransparentRecursive(c, editorMode_);
        if (auto* f = qobject_cast<SelectionFrame*>(c))
        {
            f->setEditorModeCapture(editorMode_);
            // Deselect and hide selection chrome entirely when turning editor mode off
            if (!editorMode_)
            {
                f->setSelected(false);
            }
        }
    }
    // Also update currently selected pointer
    if (!editorMode_)
    {
        selected_ = nullptr;
        dragMode_ = DragMode::None;
        emit selectionChanged(nullptr);
    }
    // Ensure the canvas repaints immediately to show/hide gridlines
    update();
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
    SelectionFrame* frame = new SelectionFrame(type, this);
    if (frame)
    {
        const QPoint pos = event->position().toPoint();
        // Provide a reasonable default size for various widgets
        if (frame->child() && frame->child()->sizeHint().isValid())
        {
            frame->child()->resize(frame->child()->sizeHint());
        }
        else
        {
            if (frame->child()) frame->child()->resize(200, 200);
        }

        frame->move(pos);
        if (frame->child()) frame->resize(frame->child()->size());
        frame->show();
        // Apply current editor mode to the new widget subtree
        frame->setEditorModeCapture(editorMode_);
        Item item;
        item.widget = frame;
        item.type = type;
        item.position = pos;
        items_.push_back(std::move(item));
        update();
        event->acceptProposedAction();

        // Select newly dropped widget
        if (auto* prev = qobject_cast<SelectionFrame*>(selected_)) prev->setSelected(false);
        selected_ = frame;
        frame->setSelected(true);
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
    if (editorMode_)
    {
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
}

void Canvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // nothing for per-widget frames
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

Canvas::DragMode Canvas::hitTestSelectionAt(const QPoint& pos)
{
    if (!selected_) return DragMode::None;
    if (auto* frame = qobject_cast<SelectionFrame*>(selected_))
    {
        return static_cast<DragMode>(frame->hitTestCanvasPos(pos));
    }
    return DragMode::None;
}

void Canvas::mousePressEvent(QMouseEvent* event)
{
    if (!editorMode_)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint pos = event->pos();
    // Determine if clicking on a top-level child widget using stored layout (works with transparent children)
    QWidget* topLevel = topLevelWidgetAt(pos);
    if (topLevel)
    {
        if (auto* prev = qobject_cast<SelectionFrame*>(selected_)) prev->setSelected(false);
        selected_ = topLevel;
        if (auto* f = qobject_cast<SelectionFrame*>(selected_)) f->setSelected(true);
        selectedRect_ = widgetRect(selected_);
        dragMode_ = hitTestSelectionAt(pos);
        dragStartPos_ = pos;
        dragStartRect_ = selectedRect_;
        update();
        emit selectionChanged(selected_);
        return;
    }

    // Not clicking inside any widget's rect. If we already have a selection, allow grabs on handles even if they extend outside the rect
    if (selected_)
    {
        DragMode hm = hitTestSelectionAt(pos);
        if (hm != DragMode::None)
        {
            dragMode_ = hm;
            dragStartPos_ = pos;
            dragStartRect_ = selectedRect_;
            update();
            return;
        }
    }
    // Otherwise clear selection
    if (auto* prev = qobject_cast<SelectionFrame*>(selected_)) prev->setSelected(false);
    selected_ = nullptr;
    dragMode_ = DragMode::None;
    update();
    emit selectionChanged(nullptr);
}

void Canvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!editorMode_)
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
    update();
    
}

void Canvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (!editorMode_)
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
            if (auto* f = qobject_cast<SelectionFrame*>(selected_)) f->setSelected(false);
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

QString Canvas::getBackgroundColorHex() const
{
    return palette().color(QPalette::Window).name();
}

#include "editor/moc_canvas.cpp"
