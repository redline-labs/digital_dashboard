#ifndef DASHBOARD_EDITOR_SELECTION_OVERLAY_H
#define DASHBOARD_EDITOR_SELECTION_OVERLAY_H

#include <QWidget>
#include <QRect>

enum class DragMode
{
    None,
    Move,
    ResizeTL,
    ResizeTR,
    ResizeBL,
    ResizeBR
};

class SelectionOverlay : public QWidget
{
    Q_OBJECT
  public:
    // Visual size of handles
    constexpr static int kGrabHandleSizePx = 12;

    explicit SelectionOverlay(QWidget* parent = nullptr);

    void setSelectionRect(const QRect& rect);
    QRect selectionRect() const { return rect_; }

    // Hit-test the current selection rect's handles (and body for Move)
    DragMode hitTest(const QPoint& pos) const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QRect rect_;
};

#endif // DASHBOARD_EDITOR_SELECTION_OVERLAY_H


