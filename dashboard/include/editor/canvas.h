#ifndef DASHBOARD_EDITOR_CANVAS_H
#define DASHBOARD_EDITOR_CANVAS_H

#include <QWidget>
#include <QPoint>
#include <QMouseEvent>
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
    // Export current canvas as a window configuration with given name
    app_config_t exportAppConfig(const std::string& window_name) const;

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

    QWidget* selected_ = nullptr; // points to SelectionFrame now
    QRect selectedRect_;

    enum class DragMode { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };
    DragMode dragMode_ = DragMode::None;
    QPoint dragStartPos_;
    QRect dragStartRect_;
    bool editorMode_;
    QString backgroundColorHex_ = "#1e1e1e";

    QRect widgetRect(QWidget* w) const;
    void setMouseTransparentRecursive(QWidget* w, bool on);
    QWidget* topLevelWidgetAt(const QPoint& pos) const;
    DragMode hitTestSelectionAt(const QPoint& pos);
    void clearAll();
};

#endif // DASHBOARD_EDITOR_CANVAS_H


