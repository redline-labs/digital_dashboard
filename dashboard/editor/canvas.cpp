#include "editor/canvas.h"
#include "editor/editor_constants.h"
#include "editor/selection_frame.h"

#include "qt_helpers/widget_colors.h"
#include "dashboard/widget_identity.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QLabel>
#include <QApplication>
#include <QKeyEvent>
#include <map>
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

Canvas::Snapshot Canvas::captureDocument() const
{
    Snapshot state;
    state.doc = exportAppConfig();

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

void Canvas::applyDocument(const Snapshot& state)
{
    // Restoring used to be loadFromAppConfig(): destroy every widget, build every
    // widget. Correct, and brutal. Undoing a nudge of one static_text tore down
    // and rebuilt the CarPlay widget sitting next to it -- three zenoh
    // subscriptions, an H.264 decoder and an audio sink, gone and re-established,
    // with a visible glitch on screen and a stream that has to re-sync on the
    // next keyframe. It also invalidated every pointer anyone held across an
    // undo, and it is what made the zenoh/GUI-thread deadlock reachable at all.
    //
    // So diff instead. Widgets are matched by the object names carried in the
    // snapshot, which are stable across a restore precisely because they are
    // carried; a matched widget keeps its live object unless its configuration
    // actually differs, and a move is then just a move.

    windowName_ = state.doc.name;
    resize(state.doc.width, state.doc.height);
    setBackgroundColor(QString::fromStdString(state.doc.background_color));

    // Names are the identity. A snapshot with a name per widget is the normal
    // case; the mismatched one can only arise from a snapshot taken before the
    // names were recorded, and is handled by falling back to a full rebuild
    // rather than by guessing at a pairing.
    if (state.names.size() != state.doc.widgets.size())
    {
        SPDLOG_WARN("Snapshot has {} widgets but {} names; rebuilding rather than diffing.",
                    state.doc.widgets.size(), state.names.size());
        const std::size_t names_before = nextNameIndex_;
        loadFromAppConfig(state.doc);
        nextNameIndex_ = std::max(nextNameIndex_, names_before);
        emit selectionChanged(nullptr);
        emit historyChanged();
        return;
    }

    // What is on the canvas now, by name.
    std::map<QString, SelectionFrame*> live;
    for (const auto& frame : items_)
    {
        if (frame)
        {
            live.emplace(frame->objectName(), frame.data());
        }
    }

    std::vector<QPointer<SelectionFrame>> rebuilt;
    rebuilt.reserve(state.doc.widgets.size());
    bool selection_survived = false;

    for (std::size_t i = 0; i < state.doc.widgets.size(); ++i)
    {
        const widget_config_t& wcfg = state.doc.widgets[i];
        const QString& name = state.names[i];

        SelectionFrame* frame = nullptr;
        if (const auto it = live.find(name); it != live.end() && it->second->type() == wcfg.type)
        {
            frame = it->second;
            live.erase(it);

            // Only rebuild the child when the configuration actually changed.
            // This is the whole point: a drag, a resize or a move leaves the
            // config alone, so the widget -- and whatever it is connected to --
            // is left running.
            if (!(frame->config() == wcfg.config))
            {
                frame->applyStoredConfig(wcfg.config);
            }
        }
        else
        {
            frame = new SelectionFrame(wcfg.type, this);
            frame->applyStoredConfig(wcfg.config);
            frame->ensureChild();
            frame->show();
        }

        frame->setId(wcfg.id);
        frame->setObjectName(name);

        const QRect target(wcfg.x, wcfg.y, wcfg.width, wcfg.height);
        if (widgetRect(frame) != target)
        {
            frame->move(target.topLeft());
            frame->resize(target.size());
        }
        frame->setEditorModeCapture(editorMode_);

        if (frame == selected_)
        {
            selection_survived = true;
        }
        rebuilt.push_back(frame);
    }

    // Anything left in `live` is not in the restored document.
    for (const auto& [name, frame] : live)
    {
        frame->deleteLater();
    }

    items_ = std::move(rebuilt);

    // Keep the naming counter ahead of anything the restored document uses, so a
    // widget added next cannot collide with one that came back from an undo.
    nextNameIndex_ = std::max(nextNameIndex_, state.doc.widgets.size());

    // The selection only has to be dropped if the widget holding it is gone.
    // Otherwise it is re-emitted rather than cleared, so the properties panel
    // rebuilds its form from the restored configuration and goes on showing the
    // widget the user was working on. Undo used to blank the panel every time,
    // because every widget really had been destroyed.
    dragMode_ = DragMode::None;
    if (selection_survived)
    {
        selectedRect_ = widgetRect(selected_);
        emit selectionChanged(selected_);
    }
    else
    {
        selected_ = nullptr;
        emit selectionChanged(nullptr);
    }

    update();
    emit historyChanged();
}

void Canvas::beginEdit(EditSource source)
{
    history_.begin(source);
}

void Canvas::commitEdit()
{
    // historyChanged, not the stacks: the canvas owns telling the window that
    // undo availability or the dirty flag may have moved. EditorDocument has no
    // idea a window exists.
    history_.commit();
    emit historyChanged();
}

bool Canvas::undo()
{
    return history_.undo();
}

bool Canvas::redo()
{
    return history_.redo();
}

bool Canvas::canUndo() const
{
    return history_.canUndo();
}

bool Canvas::canRedo() const
{
    return history_.canRedo();
}

void Canvas::clearHistory()
{
    history_.clearHistory();
    emit historyChanged();
}

bool Canvas::isDirty() const
{
    return history_.isDirty();
}

void Canvas::markSaved()
{
    history_.markSaved();
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

    // find + erase, not remove_if + erase. remove_if shuffles the vector before
    // anything opens a transaction, so the snapshot the transaction takes is
    // already of the post-removal state and commitEdit() then discards the entry
    // as a no-op -- a delete that could not be undone. That went unnoticed while
    // items_ held raw pointers, because moving one leaves the source unchanged
    // and the shuffled tail still looked like a live widget; QPointer nulls
    // itself on move, which is what exposed it.
    const auto it = std::find(items_.begin(), items_.end(), frame);
    if (it == items_.end())
    {
        return false;
    }

    const auto tx = edit();
    items_.erase(it);

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
