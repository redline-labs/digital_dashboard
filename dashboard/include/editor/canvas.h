#ifndef DASHBOARD_EDITOR_CANVAS_H
#define DASHBOARD_EDITOR_CANVAS_H

#include <QWidget>
#include <QPoint>
#include <QPointer>
#include <QMouseEvent>
#include <optional>
#include <vector>

#include "dashboard/app_config.h"
#include "editor/selection_frame.h"

// No global selection overlay when using per-widget SelectionFrame

class Canvas : public QWidget
{
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);
    void setBackgroundColor(const QString& hexColor);
    // Enable/disable editor mode (selection, resize, gridlines, event interception)
    void setEditorMode(bool enabled);
    // Clear and populate from a dashboard window configuration
    void loadFromAppConfig(const app_config_t& app_cfg);
    // Export current canvas as a window configuration.
    app_config_t exportAppConfig() const;
    // Current background color hex string (e.g. "#1e1e1e")
    QString getBackgroundColorHex() const;

    // The window's name, carried through load -> save so a config keeps the one
    // it arrived with. The canvas owns it for the same reason it owns the
    // background colour: it is window state, and it has to survive an export.
    const std::string& windowName() const { return windowName_; }
    void setWindowName(std::string name) { windowName_ = std::move(name); }

    // Dialog-free, event-free editing. dropEvent is a thin wrapper over
    // addWidget, so a widget added by an agent and one added by dragging from
    // the palette go through exactly the same path.
    SelectionFrame* addWidget(widget_type_t type, const QPoint& pos, const QSize& size = QSize());
    void selectFrame(SelectionFrame* frame);
    bool removeFrame(SelectionFrame* frame);
    std::vector<SelectionFrame*> frames() const;
    bool editorMode() const { return editorMode_; }

signals:
    void selectionChanged(QWidget* selected);

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
    // Just the frame. There were two more fields here -- a cached `type` and a
    // `position` -- and both were dead weight that had already gone stale:
    // `position` was written when the widget was created and never updated by a
    // drag, editor.move or editor.resize, and nothing read it (widgetRect() asks
    // the widget). `type` duplicated SelectionFrame::type_, which every reader
    // used instead. A second copy of state that nobody maintains is where the
    // next bug comes from.
    struct Item {
        QWidget* widget;
    };

    std::vector<Item> items_;

    // Monotonic source of derived widget names. Never reset by a deletion, so a
    // name is not handed out twice within one editing session.
    std::size_t nextNameIndex_ = 0;

    std::string windowName_;

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

    QRect widgetRect(QWidget* w) const;
    void setMouseTransparentRecursive(QWidget* w, bool on);
    QWidget* topLevelWidgetAt(const QPoint& pos) const;
    DragMode hitTestSelectionAt(const QPoint& pos);
    void clearAll();
};

#endif // DASHBOARD_EDITOR_CANVAS_H


