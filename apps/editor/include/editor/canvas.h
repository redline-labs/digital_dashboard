#ifndef DASHBOARD_EDITOR_CANVAS_H
#define DASHBOARD_EDITOR_CANVAS_H

#include <QWidget>
#include <QPoint>
#include <QPointer>
#include <QMouseEvent>
#include <QSize>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "dashboard/app_config.h"
#include "editor/editor_document.h"
#include "editor/selection_frame.h"

// No global selection overlay when using per-widget SelectionFrame

class Canvas : public QWidget, private EditorDocument::Owner
{
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);
    void setBackgroundColor(const QString& hexColor);
    // Enable/disable editor mode (selection, resize, gridlines, event interception)
    void setEditorMode(bool enabled);
    // Clear and populate from one window. The document becomes just that window.
    void loadFromAppConfig(const app_config_t& app_cfg);
    // Export the window on the canvas.
    app_config_t exportAppConfig() const;
    // Current background color hex string (e.g. "#1e1e1e")
    QString getBackgroundColorHex() const;

    // ------------------------------------------------------------------ windows
    //
    // A document is one or more windows, and the canvas shows one at a time.
    // The others are held as configs rather than live widgets: building a
    // window nobody is looking at would start its subscriptions (CarPlay's
    // decoder among them) for nothing.
    void loadDocument(const dashboard_config_t& doc);
    // Every window, with the one on the canvas exactly as it stands.
    dashboard_config_t exportDocument() const;

    struct WindowSummary
    {
        std::string name;
        display_role_t display;
        scale_mode_t scale;
        int width;
        int height;
    };
    std::vector<WindowSummary> windowSummaries() const;

    std::size_t windowCount() const { return document_.windows.size(); }
    std::size_t activeWindow() const { return activeWindow_; }

    // Shows another window. Not an edit: nothing in the document changes.
    bool selectWindow(std::size_t index);

    // Adds a window and shows it. Undoable. A window with no name is given a
    // fresh one; with no display, the first one no window has yet. Refused
    // (nullopt) when every display is taken or the name is already used,
    // because either would save a file the loader rejects.
    std::optional<std::size_t> addWindow(std::string name = {},
                                         std::optional<display_role_t> display = std::nullopt,
                                         QSize size = QSize());

    // Undoable. The last window cannot be removed.
    bool removeWindow(std::size_t index);

    // The window's name, carried through load -> save so a config keeps the one
    // it arrived with. The canvas owns it for the same reason it owns the
    // background colour: it is window state, and it has to survive an export.
    const std::string& windowName() const { return windowName_; }
    void setWindowName(std::string name);

    display_role_t display() const { return display_; }
    // Choosing a display another window already has swaps the two, so the
    // document never holds two windows on one display.
    void setDisplay(display_role_t display);

    scale_mode_t scale() const { return scale_; }
    void setScale(scale_mode_t scale);

    // Dialog-free, event-free editing. dropEvent is a thin wrapper over
    // addWidget, so a widget added by an agent and one added by dragging from
    // the palette go through exactly the same path.
    SelectionFrame* addWidget(widget_type_t type, const QPoint& pos, const QSize& size = QSize());
    void selectFrame(SelectionFrame* frame);
    bool removeFrame(SelectionFrame* frame);
    std::vector<SelectionFrame*> frames() const;
    bool editorMode() const { return editorMode_; }

    // ------------------------------------------------------------- edit history
    //
    // Snapshot-based: each entry is a whole exported config, not an inverse
    // operation. With a few dozen widgets that is cheap, and it cannot drift the
    // way a set of hand-written undo/redo pairs does -- every mutation is
    // covered by construction, including the ones the agent interface makes,
    // because they all end up changing what exportAppConfig() returns.
    //
    // The mechanics live in EditorDocument; what remains here is the widget half
    // -- capturing the canvas as a document and putting one back -- plus the
    // signals the window listens to. These forward, so that every caller keeps
    // talking to the canvas rather than reaching past it into its history.
    using EditSource = EditorDocument::EditSource;

    void beginEdit(EditSource source = EditSource::Widget);
    void commitEdit();

    // Scoped form of the pair above, for the common case where an edit begins and
    // ends in one function.
    //
    // The manual pair is still there for the cases that genuinely span calls -- a
    // drag opens on mousePress and closes on mouseRelease, a window field opens on
    // a keystroke and closes on editingFinished -- but everything else was
    // matching them up by hand across six call sites, with the boundary landing
    // in a different place each time: addWidget and removeFrame wrapped
    // themselves, editor.move and editor.resize wrapped from outside, and the
    // window fields did not wrap at all until they were found not to.
    class [[nodiscard]] EditTransaction
    {
      public:
        EditTransaction(Canvas* canvas, EditSource source) : canvas_(canvas)
        {
            if (canvas_) canvas_->beginEdit(source);
        }
        ~EditTransaction()
        {
            if (canvas_) canvas_->commitEdit();
        }

        EditTransaction(const EditTransaction&) = delete;
        EditTransaction& operator=(const EditTransaction&) = delete;
        EditTransaction(EditTransaction&&) = delete;
        EditTransaction& operator=(EditTransaction&&) = delete;

      private:
        QPointer<Canvas> canvas_;
    };

    EditTransaction edit(EditSource source = EditSource::Widget)
    {
        return EditTransaction(this, source);
    }

    // Drops the history. Called after a load: undoing past it would restore the
    // previous document, which is not what anyone means by undo.
    void clearHistory();

    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();

    // True when the canvas differs from the last save (or load). Saving calls
    // markSaved() to make the current state the new baseline.
    bool isDirty() const;
    void markSaved();

signals:
    void selectionChanged(QWidget* selected);

    // Emitted whenever undo/redo availability or the dirty flag may have moved,
    // so the window can update its title and menu without polling.
    void historyChanged();

    // The window list or the window being shown may have changed: a window was
    // added, removed, selected or renamed, or had its display changed.
    void windowsChanged();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // The frames on this canvas, in document order.
    //
    // This was a vector of a one-member `struct Item { QWidget* widget; }`, left
    // over after a cached `type` and `position` were removed from it. Both had
    // gone stale before they were deleted: `position` was written when the widget
    // was created and never updated by a drag, editor.move or editor.resize, and
    // `type` duplicated SelectionFrame::type_. That history is the reason
    // geometry still lives on the widget and nowhere else -- a second copy of
    // state that nobody maintains is where the next bug comes from.
    //
    // QPointer rather than a raw pointer for the same reason selected_ is one:
    // frames are removed with deleteLater(), so a raw pointer is briefly valid
    // and then not.
    std::vector<QPointer<SelectionFrame>> items_;

    // Monotonic source of derived widget names. Never reset by a deletion, so a
    // name is not handed out twice within one editing session.
    std::size_t nextNameIndex_ = 0;

    std::string windowName_;
    display_role_t display_ = display_role_t::primary;
    scale_mode_t scale_ = scale_mode_t::fit;

    // Every window of the document. The entry at activeWindow_ is stale while
    // that window is on the canvas -- the live widgets are its truth -- and is
    // written back whenever the canvas stops showing it or the document is
    // exported.
    dashboard_config_t document_;
    std::size_t activeWindow_ = 0;

    // Rebuilds the canvas from one window, leaving document_ alone.
    void loadWindow(const app_config_t& window);

    // The configured background, verbatim. Authoritative -- the widget's palette
    // is derived from it and never read back. See setBackgroundColor().
    helpers::Color backgroundColor_;

    // QPointer, not a raw pointer: this used to rely on every deletion path
    // remembering to null it. They all did, but the next one added would not
    // have, and the failure is a dangling dereference on the next click.
    QPointer<SelectionFrame> selected_;
    QRect selectedRect_;

    enum class DragMode { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };
    DragMode dragMode_ = DragMode::None;
    QPoint dragStartPos_;
    QRect dragStartRect_;
    bool editorMode_;

    // The widget type a drag payload names, or nullopt if it names none. Shared
    // by dragEnterEvent and dropEvent so the drag is refused up front rather
    // than accepted and then discarded.
    static std::optional<widget_type_t> droppedWidgetType(const QString& mimeText);

    // The widget half of the history: the canvas as a document, and a document
    // put back onto the canvas. EditorDocument calls these; it holds the stacks.
    using Snapshot = EditorDocument::Snapshot;
    Snapshot captureDocument() const override;
    void applyDocument(const Snapshot& state) override;

    EditorDocument history_{*this};

    QRect widgetRect(QWidget* w) const;
    void setMouseTransparentRecursive(QWidget* w, bool on);
    QWidget* topLevelWidgetAt(const QPoint& pos) const;
    DragMode hitTestSelectionAt(const QPoint& pos);
    void clearAll();
};

#endif // DASHBOARD_EDITOR_CANVAS_H


