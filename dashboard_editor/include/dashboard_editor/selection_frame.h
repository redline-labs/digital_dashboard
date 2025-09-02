#ifndef DASHBOARD_EDITOR_SELECTION_FRAME_H
#define DASHBOARD_EDITOR_SELECTION_FRAME_H

#include <QWidget>
#include <QRect>

#include "app_config.h"

class SelectionFrame : public QWidget
{
    Q_OBJECT
public:
    constexpr static int kGrabHandleSizePx = 12;

    explicit SelectionFrame(widget_type_t type, QWidget* child, QWidget* parent = nullptr);

    widget_type_t type() const { return type_; }
    QWidget* child() const { return child_; }
    void setChild(QWidget* newChild);

    void setSelected(bool on);
    bool isSelected() const { return selected_; }

    // Editor mode: when true, this frame captures interactions; when false, pass through to child
    void setEditorModeCapture(bool on);

    enum class Handle { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };
    // Hit-test using canvas coordinates (parent space)
    Handle hitTestCanvasPos(const QPoint& canvasPos) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    widget_type_t type_;
    QWidget* child_ = nullptr;
    bool selected_ = false;
    bool editorMode_ = true;
    QWidget* overlay_ = nullptr; // draws selection chrome above child
};

#endif // DASHBOARD_EDITOR_SELECTION_FRAME_H


