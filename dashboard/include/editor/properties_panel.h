#ifndef DASHBOARD_EDITOR_PROPERTIES_PANEL_H
#define DASHBOARD_EDITOR_PROPERTIES_PANEL_H

#include <QWidget>
#include <QPointer>

class QStackedWidget;
class QLineEdit;
class QSpinBox;
class QComboBox;
class Canvas;

class PropertiesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PropertiesPanel(QWidget* parent = nullptr);
    void setCanvas(Canvas* canvas);
    QWidget* selected() const { return selected_; }
    Canvas* canvas() const { return canvas_; }

public slots:
    void setSelectedWidget(QWidget* w);
    void syncFromCanvas();

private:
    QPointer<QWidget> selected_;

    QStackedWidget* stack_;

    // Cache of dynamically built pages keyed by widget class name
    QHash<QString, QWidget*> widgetPages_;

    // Window editors
    QWidget* windowPage_;
    QLineEdit* winNameEdit_;
    QSpinBox* winWidthSpin_;
    QSpinBox* winHeightSpin_;
    QLineEdit* winBgColorEdit_;
    Canvas* canvas_;
    bool isSyncing_;

    void showUnsupported(const QString& name);
    void buildWindowPage();
    void applyWindowEdits();
};

#endif // DASHBOARD_EDITOR_PROPERTIES_PANEL_H


