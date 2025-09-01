#ifndef DASHBOARD_EDITOR_CANVAS_H
#define DASHBOARD_EDITOR_CANVAS_H

#include <QWidget>
#include <QPoint>
#include <QMouseEvent>
#include <vector>

#include "app_config.h"
#include "dashboard_editor/selection_overlay.h"

class SelectionOverlay;

class Canvas : public QWidget
{
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);
    void setBackgroundColor(const QString& hexColor);
    void setInterceptInteractions(bool intercept);
    void replaceWidget(QWidget* oldWidget, QWidget* newWidget, const QRect& rect);

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
    struct Item {
        QWidget* widget;
        widget_type_t type; // cached type for this widget
        QPoint position; // top-left position in canvas coordinates
    };

    std::vector<Item> items_;

    QWidget* selected_ = nullptr;
    QRect selectedRect_;

    DragMode dragMode_ = DragMode::None;
    QPoint dragStartPos_;
    QRect dragStartRect_;
    bool interceptInteractions_;
    SelectionOverlay* overlay_ = nullptr;

    QWidget* createWidgetForType(const QString& typeKey, QWidget* parent);
    QRect widgetRect(QWidget* w) const;
    void updateSelectionOverlay();
    void setMouseTransparentRecursive(QWidget* w, bool on);
    QWidget* topLevelWidgetAt(const QPoint& pos) const;
    DragMode hitTestSelectionAt(const QPoint& pos);
};

#endif // DASHBOARD_EDITOR_CANVAS_H


