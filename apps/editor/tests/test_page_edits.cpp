// A page_stack's page edits as functions of a document snapshot: no canvas, no
// widgets. What each must keep loadable -- every reference to a renamed page,
// a default that would dangle, a stack that would have no pages -- and that a
// refusal changes nothing, which is what keeps a refused edit out of undo.

#include "editor/page_edits.h"

#include "dashboard/app_config.h"
#include "dashboard/widget_registry.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <string>
#include <variant>

namespace
{

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using editor::page_edits::Snapshot;
namespace edits = editor::page_edits;

// A stack with two pages in one window, and a button aimed at it from another;
// the same document as editor_test_pages.
Snapshot snapshot()
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
            - source:
                zenoh_key: nodes/grayhill_keypad/buttons
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
    Snapshot state;
    state.doc = YAML::Load(yaml).as<dashboard_config_t>();
    state.active_window = 0;
    state.names = {QStringLiteral("stack")};
    state.page_names = {{{QStringLiteral("on_first"), QStringLiteral("static_text#1")},
                         {QStringLiteral("back")}}};
    return state;
}

widget_config_t& stack(Snapshot& state)
{
    return state.doc.windows[0].widgets[0];
}

PageStackWidget::config_t& stackConfig(Snapshot& state)
{
    return std::get<PageStackWidget::config_t>(stack(state).config);
}

page_command_t& commandOf(widget_config_t& widget)
{
    return std::get<PageButtonWidget::config_t>(widget.config).command;
}

void testRenameFollowsEveryReference()
{
    Snapshot state = snapshot();
    check(edits::renamePage(state, 0, 0, "home"), "a page renames");
    check(stack(state).pages[0].name == "home", "the page has its new name");
    check(stackConfig(state).default_page == "home", "the stack's default follows it");

    edits::renamePage(state, 0, 1, "detail");
    check(stackConfig(state).triggers[0].page == "detail", "a trigger follows its page");
    check(commandOf(state.doc.windows[1].widgets[0]).page == "detail",
          "a button in another window follows the page it aims at");
    check(commandOf(stack(state).pages[1].widgets[0]).page == "home",
          "a button on the stack's own page follows too");

    const Snapshot before = state;
    check(!edits::renamePage(state, 0, 0, "detail"), "a name another page has is refused");
    check(!edits::renamePage(state, 0, 0, "home"), "renaming to its own name is no change");
    check(!edits::renamePage(state, 0, 0, ""), "an empty name is refused");
    check(!edits::renamePage(state, 0, 5, "x"), "a page that does not exist is refused");
    check(!edits::renamePage(state, 1, 0, "x"), "a widget that is not a stack is refused");
    check(state == before, "and every refusal left the document as it was");
}

void testAddAndRemove()
{
    Snapshot state = snapshot();
    const auto added = edits::addPage(state, 0, "");
    check(added == 2u && stack(state).pages[2].name == "page_3", "an unnamed page gets the next free name");
    check(state.page_names[0].size() == 3, "and a slot for its widgets' names");
    check(!edits::addPage(state, 0, "first"), "a taken name is refused");

    check(edits::removePage(state, 0, 0), "a page removes");
    check(stackConfig(state).default_page.empty(), "a default naming it is cleared rather than left dangling");
    check(state.page_names[0].size() == 2 && state.page_names[0][0].front() == QStringLiteral("back"),
          "and its widgets' names go with it");

    check(edits::removePage(state, 0, 0), "down to one page");
    const Snapshot before = state;
    check(!edits::removePage(state, 0, 0), "the last page cannot be removed: a stack with none does not load");
    check(state == before, "and that refusal changed nothing");
}

void testMoves()
{
    Snapshot state = snapshot();
    check(edits::movePage(state, 0, 0, 1), "a page moves");
    check(stack(state).pages[0].name == "second" && stack(state).pages[1].name == "first",
          "the pages swapped");
    check(state.page_names[0][1].front() == QStringLiteral("on_first"), "and their widgets' names with them");
    check(!edits::movePage(state, 0, 0, 0), "a move to where it is is no change");
    check(!edits::movePage(state, 0, 0, 7), "a move past the end is refused");

    state = snapshot();
    check(edits::moveWidgetToPage(state, 0, 0, 0, 1), "a widget moves to another page");
    check(stack(state).pages[0].widgets.size() == 1 && stack(state).pages[1].widgets.size() == 2,
          "off one page and onto the other");
    check(stack(state).pages[1].widgets[1].id == "on_first", "at the end, as itself");
    check(state.page_names[0][1].back() == QStringLiteral("on_first") && state.page_names[0][0].size() == 1,
          "with its name, so the canvas moves it rather than rebuilding it");
    check(!edits::moveWidgetToPage(state, 0, 1, 0, 1), "a move to its own page is no change");

    Snapshot unnamed = snapshot();
    unnamed.page_names[0][0].clear();
    check(!edits::moveWidgetToPage(unnamed, 0, 0, 0, 1), "a widget the snapshot has no name for is refused");
}

void testInCycle()
{
    Snapshot state = snapshot();
    check(!edits::setPageInCycle(state, 0, 0, true), "setting what is already set is no change");
    check(edits::setPageInCycle(state, 0, 1, true), "a page joins the cycle");
    check(stack(state).pages[1].in_cycle, "and is in it");
}

}  // namespace

int main()
{
    testRenameFollowsEveryReference();
    testAddAndRemove();
    testMoves();
    testInCycle();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
