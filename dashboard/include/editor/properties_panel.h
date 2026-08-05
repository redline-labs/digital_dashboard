#ifndef DASHBOARD_EDITOR_PROPERTIES_PANEL_H
#define DASHBOARD_EDITOR_PROPERTIES_PANEL_H

#include <QWidget>
#include <QPointer>

class QStackedWidget;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QLabel;
class Canvas;
class SelectionFrame;

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

    // Names what is currently being edited: the widget's friendly type over the
    // selector that addresses it, or "Window" when nothing is selected.
    QLabel* heading_ = nullptr;
    QLabel* subheading_ = nullptr;
    void showHeading(SelectionFrame* frame);

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

    // Pushes the window fields onto the canvas and opens a history entry.
    // Called per keystroke; commitWindowEdits() closes the entry when the field
    // is finished, so an edited field costs one undo step rather than one per
    // character.
    void applyWindowEdits();
    void commitWindowEdits();
};

#endif // DASHBOARD_EDITOR_PROPERTIES_PANEL_H


