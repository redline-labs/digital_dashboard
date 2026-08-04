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

    // The form for the current selection. Rebuilt on every selection change and
    // owned only until the next one.
    //
    // This used to be a cache keyed by widget *class* name, which meant two
    // widgets of the same type shared one page: selecting the second showed the
    // first's values, and Apply then wrote them into the second. A per-instance
    // cache would fix the values but still leak a page per widget, and the form
    // has to be re-read from the live config on selection anyway -- so there is
    // nothing worth keeping between selections.
    QPointer<QWidget> currentPage_;

    // Window editors
    QWidget* windowPage_;
    QLineEdit* winNameEdit_;
    QSpinBox* winWidthSpin_;
    QSpinBox* winHeightSpin_;
    QLineEdit* winBgColorEdit_;
    Canvas* canvas_;
    bool isSyncing_;

    void discardCurrentPage();
    void showPage(QWidget* page);
    void showUnsupported(const QString& name);
    void buildWindowPage();
    void applyWindowEdits();
};

#endif // DASHBOARD_EDITOR_PROPERTIES_PANEL_H


