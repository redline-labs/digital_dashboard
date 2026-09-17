// Editing a page_stack's pages in the editor: the widgets on them as live frames,
// the scope that clicks and drops go into, the page operations, and undo across
// all of it.
//
// The failures this is guarding against are quiet ones. An undo that rebuilds a
// page's widgets looks right and reconnects CarPlay; a renamed page that leaves a
// button pointing at the old name saves a file the dashboard refuses; a drop
// that lands on the window instead of the page puts a widget where the stack
// then hides it.
//
// Labelled gui: it constructs Qt widgets, on the offscreen platform.

#include "agent_control/input.h"
#include "editor/canvas.h"
#include "editor/properties_panel.h"
#include "editor/selection_frame.h"

#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string toYaml(const dashboard_config_t& doc)
{
    YAML::Emitter out;
    out << YAML::convert<dashboard_config_t>::encode(doc);
    return out.c_str();
}

// A stack with two pages in one window, and a button aimed at it from another.
dashboard_config_t document()
{
    const char* yaml = R"(
name: paged
windows:
  - name: main
    width: 800
    height: 600
    widgets:
      - id: stack
        type: page_stack
        x: 100
        y: 50
        width: 400
        height: 300
        config:
          default_page: first
          triggers:
            - zenoh_key: nodes/grayhill_keypad/buttons
              schema_type: GrayhillButtons
              expression: bit(buttons1To8, 0)
              action: go_to
              page: second
        pages:
          - name: first
            widgets:
              - id: on_first
                type: static_text
                x: 10
                y: 20
                width: 120
                height: 40
                config: {text: first}
              - type: static_text
                x: 200
                y: 20
                width: 120
                height: 40
                config: {text: unnamed}
          - name: second
            in_cycle: false
            widgets:
              - id: back
                type: page_button
                x: 5
                y: 6
                width: 100
                height: 40
                config:
                  label: Back
                  command: {target: stack, action: go_to, page: first}
  - name: other
    display: secondary
    widgets:
      - id: remote
        type: page_button
        config:
          command: {target: stack, action: go_to, page: second}
)";
    return YAML::Load(yaml).as<dashboard_config_t>();
}

SelectionFrame* stackOf(Canvas& canvas)
{
    for (SelectionFrame* frame : canvas.frames())
    {
        if (frame->isContainer()) return frame;
    }
    return nullptr;
}

SelectionFrame* childNamed(SelectionFrame* stack, const QString& name)
{
    for (std::size_t p = 0; p < stack->pageCount(); ++p)
    {
        for (SelectionFrame* child : stack->pageFrames(p))
        {
            if (child->objectName() == name) return child;
        }
    }
    return nullptr;
}

void pump()
{
    QApplication::processEvents();
    QApplication::processEvents();
}

void mouse(QWidget* target, QEvent::Type type, QPoint pos)
{
    QMouseEvent event(type, QPointF(pos), target->mapToGlobal(QPointF(pos)), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

void testLoadBuildsLiveFramesAndSavesUnchanged()
{
    Canvas canvas;
    const dashboard_config_t doc = document();
    canvas.loadDocument(doc);

    SelectionFrame* stack = stackOf(canvas);
    check(stack != nullptr && stack->pageCount() == 2, "the stack loads with both pages");
    if (!stack) return;

    SelectionFrame* first = childNamed(stack, "on_first");
    SelectionFrame* unnamed = childNamed(stack, "stack:first:static_text#1");
    SelectionFrame* back = childNamed(stack, "back");
    check(first && unnamed && back, "page widgets are frames named by id, or by the dashboard's rule");
    if (!(first && unnamed && back)) return;

    check(first->parentWidget() == stack, "a page widget's frame lives inside its stack");
    check(first->geometry() == QRect(10, 20, 120, 40), "and is placed relative to it");
    check(!first->isHidden() && back->isHidden(), "only the previewed page is shown");
    check(canvas.frames().size() == 1 && canvas.allFrames().size() == 4,
          "frames() is the window's widgets; allFrames() adds the pages'");

    check(canvas.exportDocument() == doc, "load -> export is lossless");
    check(toYaml(canvas.exportDocument()) == toYaml(doc), "and byte-identical when written");
}

void testUndoingAMoveOnAPageRebuildsNothing()
{
    Canvas canvas;
    const dashboard_config_t doc = document();
    canvas.loadDocument(doc);
    canvas.clearHistory();
    SelectionFrame* stack = stackOf(canvas);
    QPointer<SelectionFrame> first = childNamed(stack, "on_first");
    QPointer<SelectionFrame> unnamed = childNamed(stack, "stack:first:static_text#1");
    if (!first || !unnamed)
    {
        check(false, "fixture frames exist");
        return;
    }
    QWidget* const first_widget = first->child();
    QWidget* const unnamed_widget = unnamed->child();

    {
        const auto tx = canvas.edit();
        first->move(60, 70);
    }
    check(canvas.exportDocument().windows[0].widgets[0].pages[0].widgets[0].x == 60,
          "a move on a page reaches the export");
    check(canvas.undo(), "the move is undone");
    pump();
    check(canvas.exportDocument() == doc, "undo restores the document");
    // Compared against what the stack holds now: a replaced frame is only
    // deleteLater'd, so the old pointers would still look alive.
    check(childNamed(stack, "on_first") == first && first->child() == first_widget &&
              childNamed(stack, "stack:first:static_text#1") == unnamed && unnamed->child() == unnamed_widget,
          "undoing a move on a page rebuilt no widget on it");
    check(canvas.redo() && first && first->pos() == QPoint(60, 70), "redo moves it again, same frame");
}

void testAddAndRemoveOnAPage()
{
    Canvas canvas;
    const dashboard_config_t doc = document();
    canvas.loadDocument(doc);
    canvas.clearHistory();
    SelectionFrame* stack = stackOf(canvas);

    SelectionFrame* added = canvas.addWidgetToPage(stack, 1, widget_type_t::static_text, QPoint(30, 40), QSize(80, 20));
    check(added != nullptr, "a widget can be added to a page");
    if (!added) return;
    const QString name = added->objectName();
    check(name.startsWith("stack:second:static_text#"), "named on the page rule");
    check(canvas.scope() == stack, "adding to a page puts its stack in scope");
    check(stack->shownPage() == 1 && !added->isHidden(), "and shows the page it went on");

    const dashboard_config_t after = canvas.exportDocument();
    const auto& page = after.windows[0].widgets[0].pages[1].widgets;
    check(page.size() == 2 && page[1].x == 30 && page[1].width == 80, "it is saved on that page, stack-relative");

    check(canvas.addWidgetToPage(stack, 0, widget_type_t::page_stack, QPoint(0, 0)) == nullptr,
          "a page_stack cannot be added to a page");

    check(canvas.undo(), "the add is undone");
    pump();
    check(canvas.exportDocument() == doc, "undo removes it");
    check(canvas.redo(), "redo");
    pump();
    check(childNamed(stack, name) != nullptr, "redo brings it back under the same name");

    SelectionFrame* first = childNamed(stack, "on_first");
    canvas.selectFrame(first);
    check(canvas.removeFrame(first), "a page widget can be removed");
    pump();
    check(canvas.exportDocument().windows[0].widgets[0].pages[0].widgets.size() == 1, "and is gone from the page");
    check(canvas.scope() == stack, "removing it keeps editing the stack");
    check(canvas.undo(), "the removal is undone");
    pump();
    check(childNamed(stack, "on_first") != nullptr, "undo restores it");
}

void testPageOperationsAreUndoable()
{
    Canvas canvas;
    const dashboard_config_t doc = document();
    canvas.loadDocument(doc);
    canvas.clearHistory();
    SelectionFrame* stack = stackOf(canvas);
    QPointer<SelectionFrame> first = childNamed(stack, "on_first");

    const auto added = canvas.addPage(stack);
    check(added && *added == 2 && stack->pageCount() == 3 && stack->pageName(2) == "page_3",
          "addPage appends a freshly named page");
    check(stack->shownPage() == 2, "and shows it");
    check(!canvas.addPage(stack, "first"), "a repeated name is refused");
    check(canvas.undo(), "undo the add");
    check(canvas.exportDocument() == doc, "back to the document");
    check(childNamed(stack, "on_first") == first, "adding and undoing a page rebuilt nothing on the others");

    check(canvas.renamePage(stack, 0, "home"), "rename a page");
    dashboard_config_t renamed = canvas.exportDocument();
    const auto& stack_cfg = std::get<PageStackWidget::config_t>(renamed.windows[0].widgets[0].config);
    check(renamed.windows[0].widgets[0].pages[0].name == "home", "the page has its new name");
    check(stack_cfg.default_page == "home", "the stack's default_page follows it");
    const auto& back = std::get<PageButtonWidget::config_t>(renamed.windows[0].widgets[0].pages[1].widgets[0].config);
    check(back.command.page == "home", "a button on the stack's own page follows it");
    const auto& remote = std::get<PageButtonWidget::config_t>(renamed.windows[1].widgets[0].config);
    check(remote.command.page == "second", "a button naming a different page is left alone");
    check(!canvas.renamePage(stack, 0, "second"), "renaming onto another page's name is refused");
    check(!canvas.renamePage(stack, 0, ""), "an empty name is refused");

    check(canvas.renamePage(stack, 1, "vehicle"), "rename the other page");
    renamed = canvas.exportDocument();
    const auto& trigger = std::get<PageStackWidget::config_t>(renamed.windows[0].widgets[0].config).triggers[0];
    check(trigger.page == "vehicle", "a trigger going to the page follows it");
    check(std::get<PageButtonWidget::config_t>(renamed.windows[1].widgets[0].config).command.page == "vehicle",
          "and so does a button in another window");
    check(canvas.undo() && canvas.undo(), "undo both renames");
    check(canvas.exportDocument() == doc, "back to the document");

    check(canvas.setPageInCycle(stack, 1, true) && stack->pageInCycle(1), "a page can join the cycle");
    check(canvas.movePage(stack, 1, 0) && stack->pageName(0) == "second", "pages can be reordered");
    check(childNamed(stack, "back") && stack->locateChild(childNamed(stack, "back"))->first == 0,
          "and their widgets go with them");
    check(canvas.undo() && canvas.undo(), "undo both");
    check(canvas.exportDocument() == doc, "back to the document");

    check(canvas.removePage(stack, 0), "a page can be removed");
    renamed = canvas.exportDocument();
    check(renamed.windows[0].widgets[0].pages.size() == 1, "one page left");
    check(std::get<PageStackWidget::config_t>(renamed.windows[0].widgets[0].config).default_page.empty(),
          "a default_page naming the removed page is cleared rather than left dangling");
    check(!canvas.removePage(stack, 0), "the last page cannot be removed");
    check(canvas.undo(), "undo the removal");
    pump();
    check(canvas.exportDocument() == doc, "the page and its widgets come back");

    SelectionFrame* unnamed = childNamed(stack, "stack:first:static_text#1");
    check(canvas.moveToPage(unnamed, 1), "a widget can move to another page");
    renamed = canvas.exportDocument();
    check(renamed.windows[0].widgets[0].pages[0].widgets.size() == 1 &&
              renamed.windows[0].widgets[0].pages[1].widgets.size() == 2,
          "it leaves one page and joins the other");
    check(childNamed(stack, "stack:first:static_text#1") == unnamed, "as the same frame, under the same name");
    check(canvas.scope() == stack && stack->shownPage() == 1, "and the editor follows it");
    check(canvas.undo(), "undo the move");
    check(canvas.exportDocument() == doc, "back to the document");
}

void testClicksAndTheScope()
{
    Canvas canvas;
    canvas.loadDocument(document());
    canvas.show();
    pump();
    SelectionFrame* stack = stackOf(canvas);
    SelectionFrame* first = childNamed(stack, "on_first");

    // (100,50) is the stack's corner; on_first is at (10,20)+(120x40) inside it.
    const QPoint on_first_in_canvas(100 + 30, 50 + 30);

    QPointer<QWidget> seen;
    QObject::connect(&canvas, &Canvas::selectionChanged, &canvas, [&seen](QWidget* w) { seen = w; });

    mouse(&canvas, QEvent::MouseButtonPress, on_first_in_canvas);
    mouse(&canvas, QEvent::MouseButtonRelease, on_first_in_canvas);
    check(seen == stack && canvas.scope() == nullptr, "a single click selects the stack, not what is on it");

    mouse(&canvas, QEvent::MouseButtonDblClick, on_first_in_canvas);
    check(canvas.scope() == stack, "a double-click goes inside the stack");
    check(seen == first, "and selects the widget under the pointer");

    mouse(&canvas, QEvent::MouseButtonPress, QPoint(100 + 380, 50 + 280));
    mouse(&canvas, QEvent::MouseButtonRelease, QPoint(100 + 380, 50 + 280));
    check(seen == stack && canvas.scope() == stack, "clicking empty space inside the stack selects the stack, still inside");

    mouse(&canvas, QEvent::MouseButtonPress, on_first_in_canvas);
    check(seen == first, "inside, a single click reaches the page's widget");

    // Drag it 15px right.
    mouse(&canvas, QEvent::MouseMove, on_first_in_canvas + QPoint(15, 0));
    mouse(&canvas, QEvent::MouseButtonRelease, on_first_in_canvas + QPoint(15, 0));
    check(first->pos() == QPoint(25, 20), "dragging a page's widget moves it within the stack");

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    check(seen == stack && canvas.scope() == nullptr, "Escape goes out one level, to the stack");

    canvas.selectFrame(first);
    check(canvas.scope() == stack, "selecting a page's widget directly also goes inside");
    mouse(&canvas, QEvent::MouseButtonPress, QPoint(5, 5));
    mouse(&canvas, QEvent::MouseButtonRelease, QPoint(5, 5));
    check(seen == nullptr && canvas.scope() == nullptr, "a click outside the stack leaves it");

    canvas.selectFrame(stack);
    QKeyEvent page_down(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &page_down);
    check(stack->shownPage() == 1, "PageDown shows the next page of the selected stack");
}

// Through the same enter-then-drop sequence a real palette drag produces; a bare
// drop event is not delivered to a widget that never accepted the drag.
void drop(Canvas& canvas, widget_type_t type, QPoint pos)
{
    const QByteArray key = QByteArray::fromStdString(std::string(reflection::enum_to_string(type)));
    const auto result = agent_control::sendDrop(&canvas, QPointF(pos), {{QStringLiteral("text/plain"), key}},
                                                Qt::CopyAction);
    check(result.has_value(), "the drop is delivered");
}

void testDropsLandOnTheShownPage()
{
    Canvas canvas;
    canvas.loadDocument(document());
    SelectionFrame* stack = stackOf(canvas);
    stack->showPage(1);

    drop(canvas, widget_type_t::static_text, QPoint(100 + 50, 50 + 60));
    dashboard_config_t doc = canvas.exportDocument();
    check(doc.windows[0].widgets.size() == 1, "a drop on a stack does not land on the window");
    const auto& page = doc.windows[0].widgets[0].pages[1].widgets;
    check(page.size() == 2 && page[1].x == 50 && page[1].y == 60,
          "it lands on the page being shown, at the stack-relative position");
    check(canvas.scope() == stack, "and the editor goes inside the stack");

    drop(canvas, widget_type_t::page_stack, QPoint(100 + 50, 50 + 60));
    doc = canvas.exportDocument();
    check(doc.windows[0].widgets.size() == 2 && doc.windows[0].widgets[1].type == widget_type_t::page_stack,
          "a page_stack dropped on a stack lands on the window instead");

    drop(canvas, widget_type_t::static_text, QPoint(700, 500));
    check(canvas.exportDocument().windows[0].widgets.size() == 3, "a drop outside every stack lands on the window");
}

void testNewStacksGetTheirOwnIds()
{
    Canvas canvas;
    canvas.loadFromAppConfig(app_config_t{});
    SelectionFrame* a = canvas.addWidget(widget_type_t::page_stack, QPoint(0, 0), QSize(200, 100));
    SelectionFrame* b = canvas.addWidget(widget_type_t::page_stack, QPoint(300, 0), QSize(200, 100));
    check(a && b && a->id() == "pages" && b->id() == "pages_2", "each new stack gets a free id");
    check(a && a->pageCount() == 1, "and one page");

    SelectionFrame* child = canvas.addWidgetToPage(a, 0, widget_type_t::static_text, QPoint(5, 5));
    check(child && child->objectName().startsWith("pages:main:"), "its widgets are named after that id");

    const auto issues = validate_app_config(YAML::Load(toYaml(canvas.exportDocument())));
    std::string text;
    bool error = false;
    for (const auto& issue : issues)
    {
        error = error || issue.severity == config_codec::Issue::Severity::error;
        text += "\n  " + issue.path + ": " + issue.message;
    }
    check(!error, "a layout built from the palette loads, stacks and all:" + text);
}

void testConfigEditsKeepThePages()
{
    Canvas canvas;
    canvas.loadDocument(document());
    canvas.show();
    SelectionFrame* stack = stackOf(canvas);
    QPointer<SelectionFrame> first = childNamed(stack, "on_first");
    QWidget* const widget = first->child();

    PageStackConfig_t changed;
    changed.default_page = "second";
    {
        const auto tx = canvas.edit();
        check(stack->applyConfig(changed), "the stack takes a new config");
    }
    pump();
    check(childNamed(stack, "on_first") == first && first->child() == widget,
          "a stack config change leaves its pages' widgets alone");
    check(first && !first->isHidden(), "and they are still drawn above the stack's preview");
    check(canvas.exportDocument().windows[0].widgets[0].pages[0].widgets.size() == 2, "and saved");
}

void testThePropertiesPanelEditsPages()
{
    Canvas canvas;
    canvas.loadDocument(document());
    canvas.clearHistory();
    PropertiesPanel panel;
    panel.setCanvas(&canvas);
    QObject::connect(&canvas, &Canvas::selectionChanged, &panel, &PropertiesPanel::setSelectedWidget);

    SelectionFrame* stack = stackOf(canvas);
    canvas.selectFrame(stack);

    auto* list = panel.findChild<QListWidget*>("pages:list");
    check(list && list->count() == 2, "a stack's properties list its pages");
    if (!list) return;

    list->setCurrentRow(1);
    check(stack->shownPage() == 1, "choosing a page previews it");

    auto* add = panel.findChild<QPushButton*>("pages:add");
    check(add != nullptr, "there is an Add button");
    if (add) add->click();
    pump();
    check(stack->pageCount() == 3, "Add adds a page");
    list = panel.findChild<QListWidget*>("pages:list");
    check(list && list->count() == 3, "and the list shows it");

    auto* name = panel.findChild<QLineEdit*>("pages:name");
    if (list && name)
    {
        list->setCurrentRow(0);
        name->setText("home");
        emit name->editingFinished();
        pump();
    }
    check(stack->pageName(0) == "home", "editing the name renames the page");

    auto* cycle = panel.findChild<QCheckBox*>("pages:in_cycle");
    list = panel.findChild<QListWidget*>("pages:list");
    if (list && cycle)
    {
        list->setCurrentRow(1);
        cycle = panel.findChild<QCheckBox*>("pages:in_cycle");
        cycle->setChecked(true);
        pump();
    }
    check(stack->pageInCycle(1), "the checkbox puts a page in the cycle");

    check(canvas.undo() && canvas.undo() && canvas.undo(), "each panel edit is one undo step");
    check(canvas.exportDocument() == document(), "and undoing them all restores the document");

    SelectionFrame* back = childNamed(stack, "back");
    canvas.selectFrame(back);
    check(panel.findChild<QWidget*>("page:move_to") != nullptr, "a page widget's properties say which page it is on");
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testLoadBuildsLiveFramesAndSavesUnchanged();
    testUndoingAMoveOnAPageRebuildsNothing();
    testAddAndRemoveOnAPage();
    testPageOperationsAreUndoable();
    testClicksAndTheScope();
    testDropsLandOnTheShownPage();
    testNewStacksGetTheirOwnIds();
    testConfigEditsKeepThePages();
    testThePropertiesPanelEditsPages();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
