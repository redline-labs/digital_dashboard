#include "editor/canvas.h"
#include "editor/editor_constants.h"
#include "editor/selection_frame.h"

#include "dashboard/widget_colors.h"
#include "dashboard/widget_identity.h"

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
    for (auto& frame : items_)
    {
        if (frame)
        {
            frame->deleteLater();
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

    // Canvas adopts window name, size and background color
    windowName_ = app_cfg.name;
    resize(app_cfg.width, app_cfg.height);
    setBackgroundColor(QString::fromStdString(app_cfg.background_color));

    // Create and place widgets per config.
    //
    // The naming index counts config entries, not successfully created frames.
    // MainWindow does the same, and it has to: a config with one unknown widget
    // would otherwise give every widget after it a different derived
    // "<type>#<index>" in the editor than in the dashboard -- which is exactly
    // what widget_identity.h exists to prevent.
    std::size_t config_index = 0;
    for (const auto& wcfg : app_cfg.widgets)
    {
        const std::size_t this_index = config_index++;

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

        // Carry the config's id onto the frame so it survives a load/save round
        // trip and so the agent control interface can address it. When the
        // config has no id, fall back to the same derived "<type>#<index>" name
        // the dashboard uses, so one selector addresses the same widget in both
        // apps. setId() only stores a non-empty id, so the fallback names the
        // frame without inventing an id that would then be written to the YAML.
        frame->setId(wcfg.id);
        frame->setObjectName(dashboard::widgetObjectName(wcfg, this_index));

        // Apply typed widget configuration. A mismatch here means the config's
        // `type` and its `config` block disagree, which the YAML decoder should
        // have caught -- say so rather than silently showing a default widget.
        std::visit([&](auto const& cfg){
            if constexpr (!std::is_same_v<std::decay_t<decltype(cfg)>, std::monostate>)
            {
                if (!frame->applyConfig(cfg))
                {
                    SPDLOG_ERROR("Widget '{}' kept its default configuration.",
                                 frame->objectName().toStdString());
                }
            }
        }, wcfg.config);

        // A config block built the child above. A widget with no `config:` key
        // has none yet, so give it the default one -- the frame is constructed
        // childless so that the common case does not build a widget only to
        // replace it.
        frame->ensureChild();

        // If child provides a size hint, prefer it, otherwise use config size
        QSize targetSize(wcfg.width, wcfg.height);

        frame->move(wcfg.x, wcfg.y);
        if (frame->child()) frame->child()->resize(targetSize);
        frame->resize(targetSize);
        frame->show();

        // Apply editor mode mouse transparency
        frame->setEditorModeCapture(editorMode_);

        items_.push_back(frame);
    }

    // Continue naming past whatever the file used, so a widget added right after
    // a load cannot collide with one that came out of it.
    nextNameIndex_ = config_index;

    // No selection after load
    selected_ = nullptr;
    dragMode_ = DragMode::None;
    update();
}

// ------------------------------------------------------------------ history

Canvas::Snapshot Canvas::snapshot() const
{
    Snapshot state;

    YAML::Emitter out;
    out << YAML::convert<app_config_t>::encode(exportAppConfig());
    state.yaml = out.c_str();

    state.names.reserve(items_.size());
    for (const auto& frame : items_)
    {
        if (frame)
        {
            state.names.push_back(frame->objectName());
        }
    }

    return state;
}

void Canvas::restore(const Snapshot& state)
{
    // loadFromAppConfig resets the naming counter, which would let a restored
    // state hand out a name a later widget already has. Keep the counter moving
    // forward across an undo.
    const std::size_t names_before = nextNameIndex_;
    loadFromAppConfig(YAML::Load(state.yaml).as<app_config_t>());
    nextNameIndex_ = std::max(nextNameIndex_, names_before);

    // Put the names back. loadFromAppConfig derives them from config position,
    // so without this a widget created as static_text#4 would come back from an
    // undo as static_text#3 -- silently invalidating any selector held on it.
    if (state.names.size() == items_.size())
    {
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i])
            {
                items_[i]->setObjectName(state.names[i]);
            }
        }
    }
    else
    {
        SPDLOG_WARN("Restored {} widgets but had {} names; leaving the derived ones.",
                    items_.size(), state.names.size());
    }

    emit selectionChanged(nullptr);
    emit historyChanged();
}

void Canvas::beginEdit(EditSource source)
{
    // An edit already open from somewhere else is closed, not joined. Otherwise
    // it absorbs this one and the two come back as a single undo step -- see
    // EditSource. Committing first also means the entry it records ends at the
    // right place rather than trailing into whatever follows.
    if (pending_edit_ && pending_source_ != source)
    {
        commitEdit();
    }

    // Nested or repeated begins collapse into the outermost one, so a drag that
    // arrives as press/move/move/release records a single entry.
    if (!pending_edit_)
    {
        pending_edit_ = snapshot();
        pending_source_ = source;
    }
}

void Canvas::commitEdit()
{
    if (!pending_edit_)
    {
        return;
    }

    const Snapshot before = *pending_edit_;
    pending_edit_.reset();

    // Nothing actually changed -- a drag that ended where it started, an Apply
    // that set every field to what it already was. Recording it would mean an
    // undo that appears to do nothing.
    if (before == snapshot())
    {
        return;
    }

    undo_stack_.push_back(before);
    if (undo_stack_.size() > kMaxHistory)
    {
        undo_stack_.erase(undo_stack_.begin());
    }

    // A new edit invalidates the redo branch, as everywhere else.
    redo_stack_.clear();

    emit historyChanged();
}

bool Canvas::undo()
{
    if (undo_stack_.empty())
    {
        return false;
    }

    redo_stack_.push_back(snapshot());
    const Snapshot target = undo_stack_.back();
    undo_stack_.pop_back();
    restore(target);
    return true;
}

bool Canvas::redo()
{
    if (redo_stack_.empty())
    {
        return false;
    }

    undo_stack_.push_back(snapshot());
    const Snapshot target = redo_stack_.back();
    redo_stack_.pop_back();
    restore(target);
    return true;
}

void Canvas::clearHistory()
{
    undo_stack_.clear();
    redo_stack_.clear();
    pending_edit_.reset();
    emit historyChanged();
}

bool Canvas::isDirty() const
{
    return !(snapshot() == saved_snapshot_);
}

void Canvas::markSaved()
{
    saved_snapshot_ = snapshot();
    emit historyChanged();
}

app_config_t Canvas::exportAppConfig() const
{
    app_config_t cfg;
    cfg.name = windowName_;
    cfg.width = static_cast<uint16_t>(width());
    cfg.height = static_cast<uint16_t>(height());
    cfg.background_color = getBackgroundColorHex().toStdString();

    for (const auto& frame : items_)
    {
        if (!frame) continue;
        cfg.widgets.push_back(frame->toWidgetConfig(widgetRect(frame)));
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
    // Keep the configured string, and derive the palette from it. The palette
    // used to be the only copy, with getBackgroundColorHex() reading it back
    // through QColor::name() -- which is lossy in both directions. "#112233ff"
    // came back as "#2233ff": the wrong colour, saved over the right one, on any
    // load-then-save. QColor::name() also drops alpha entirely (it defaults to
    // HexRgb), so even a correctly parsed 8-digit colour could not survive the
    // trip. The palette is now write-only -- nothing reads a colour back out of
    // Qt.
    backgroundColor_ = hexColor.toStdString();

    QPalette pal = palette();
    pal.setColor(QPalette::Window, dashboard::toQColor(backgroundColor_));
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
    // Accept only text that actually names a widget type. Accepting any text at
    // all meant a drag from a browser or a file manager was welcomed here and
    // then blew up in dropEvent, where the type lookup used to throw straight
    // out of a Qt event handler -- Qt is not exception-safe across notify(), so
    // that terminated the editor.
    if (event->mimeData()->hasText() && droppedWidgetType(event->mimeData()->text()).has_value())
    {
        event->acceptProposedAction();
    }
}

std::optional<widget_type_t> Canvas::droppedWidgetType(const QString& mimeText)
{
    return reflection::enum_traits<widget_type_t>::try_from_string(mimeText.toStdString());
}

SelectionFrame* Canvas::addWidget(widget_type_t type, const QPoint& pos, const QSize& size)
{
    if (type == widget_type_t::unknown)
    {
        return nullptr;
    }

    const auto tx = edit();

    SelectionFrame* frame = new SelectionFrame(type, this);
    if (!frame)
    {
        return nullptr;
    }

    // Nothing here has a config to apply -- a palette drop and an agent add both
    // want the widget's own defaults -- so this is the path that asks for them.
    frame->ensureChild();

    // Name it on the same rule as a loaded widget, so something just added is
    // immediately addressable rather than only after a save and reload.
    //
    // The index comes from a counter that only ever goes up, not from
    // items_.size(). With the size, adding three widgets and deleting the middle
    // one left the next addition reusing a live name -- and an agent selector
    // that matches two widgets is an AMBIGUOUS_SELECTOR error, not a coin toss.
    widget_config_t naming_cfg;
    naming_cfg.type = type;
    frame->setObjectName(dashboard::widgetObjectName(naming_cfg, nextNameIndex_++));

    if (frame->child())
    {
        if (size.isValid() && !size.isEmpty())
        {
            frame->child()->resize(size);
        }
        else if (frame->child()->sizeHint().isValid())
        {
            frame->child()->resize(frame->child()->sizeHint());
        }
        else
        {
            frame->child()->resize(200, 200);
        }
    }

    frame->move(pos);
    if (frame->child()) frame->resize(frame->child()->size());
    frame->show();
    // Apply current editor mode to the new widget subtree
    frame->setEditorModeCapture(editorMode_);

    items_.push_back(frame);
    update();

    // The transaction closes when this returns, after the selection is set.
    // Selection is not part of the document, so it makes no difference to what
    // gets recorded.
    selectFrame(frame);
    return frame;
}

void Canvas::selectFrame(SelectionFrame* frame)
{
    if (auto* prev = qobject_cast<SelectionFrame*>(selected_)) prev->setSelected(false);
    selected_ = frame;
    if (frame != nullptr)
    {
        frame->setSelected(true);
        selectedRect_ = widgetRect(selected_);
    }
    dragMode_ = DragMode::None;
    emit selectionChanged(selected_);
}

bool Canvas::removeFrame(SelectionFrame* frame)
{
    if (frame == nullptr)
    {
        return false;
    }

    const auto it = std::remove_if(items_.begin(), items_.end(),
                                   [frame](const QPointer<SelectionFrame>& f) { return f == frame; });
    if (it == items_.end())
    {
        return false;
    }

    const auto tx = edit();
    items_.erase(it, items_.end());

    if (selected_ == frame)
    {
        selectFrame(nullptr);
    }
    frame->deleteLater();
    update();

    // deleteLater leaves the frame in the child list until the event loop runs,
    // but items_ is what exportAppConfig walks, so the snapshot the transaction
    // takes on the way out is already correct.
    return true;
}

std::vector<SelectionFrame*> Canvas::frames() const
{
    std::vector<SelectionFrame*> out;
    out.reserve(items_.size());
    for (const auto& frame : items_)
    {
        if (frame)
        {
            out.push_back(frame);
        }
    }
    return out;
}

void Canvas::dropEvent(QDropEvent* event)
{
    const QString typeKey = event->mimeData()->text();
    const auto type = droppedWidgetType(typeKey);
    if (!type)
    {
        SPDLOG_WARN("Ignoring a drop of '{}': not a known widget type.", typeKey.toStdString());
        return;
    }

    if (addWidget(*type, event->position().toPoint()) != nullptr)
    {
        event->acceptProposedAction();
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
        QWidget* w = *it;
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
    if (auto* frame = qobject_cast<SelectionFrame*>(topLevel))
    {
        // selectFrame() rather than a third hand-rolled copy of "deselect the
        // old, select the new, update selectedRect_, emit" -- it already does
        // all of that, and keeping one implementation is what stops the copies
        // drifting apart.
        selectFrame(frame);
        dragMode_ = hitTestSelectionAt(pos);
        dragStartPos_ = pos;
        dragStartRect_ = selectedRect_;

        // One history entry per drag, not per mouse-move: beginEdit collapses
        // repeated calls, and commitEdit on release discards it if the widget
        // ended up where it started.
        if (dragMode_ != DragMode::None)
        {
            beginEdit();
        }
        update();
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
            beginEdit();
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
    commitEdit();
}

void Canvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
    {
        // Route through removeFrame rather than inlining a second copy of the
        // erase-and-deselect logic. The inlined one had already drifted: it did
        // not go through selectFrame(), so selectedRect_ kept describing the
        // widget that had just been deleted.
        if (selected_ && removeFrame(selected_))
        {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

QString Canvas::getBackgroundColorHex() const
{
    return QString::fromStdString(backgroundColor_.value());
}

#include "editor/moc_canvas.cpp"
