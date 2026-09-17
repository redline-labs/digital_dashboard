// SPDX-License-Identifier: GPL-3.0-or-later
//
// A dashboard window with a page_stack in it: what gets built, where it goes,
// what can be seen and touched, and what a rebuild keeps.
//
// The pure navigation rules have their own test. This is the layer above them,
// where the real failures would be: a selector path that moved, a child placed
// in window coordinates instead of the stack's, a click that lands on a page
// nobody can see, or a set_config that silently deletes every page.
#include "dashboard/main_window.h"

#include "agent_control/input.h"
#include "agent_control/locator.h"
#include "dashboard/widget_methods.h"
#include "dashboard/widget_registry.h"

#include <QApplication>
#include <QPointer>

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <unistd.h>

using namespace std::chrono_literals;

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

void pump(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
}

// Unique per run: the stack is on the real bus.
std::string stackId()
{
    return "test_pages_" + std::to_string(::getpid());
}

app_config_t windowConfig()
{
    const std::string yaml = R"(
name: paged
width: 800
height: 600
widgets:
  - id: outside
    type: static_text
    x: 0
    y: 0
    width: 100
    height: 40
    config: {text: outside}
  - id: )" + stackId() + R"(
    type: page_stack
    x: 200
    y: 100
    width: 400
    height: 300
    config:
      default_page: second
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
      - name: second
        widgets:
          - type: static_text
            x: 30
            y: 40
            width: 120
            height: 40
            config: {text: second}
)";
    return YAML::Load(yaml).as<app_config_t>();
}

void testStructureNamesAndPlacement()
{
    MainWindow window(windowConfig());
    window.show();
    pump(50ms);

    auto* outside = window.findChild<QWidget*>("outside");
    auto* stack = window.findChild<PageStackWidget*>(QString::fromStdString(stackId()));
    auto* on_first = window.findChild<QWidget*>("on_first");
    const QString derived = QString::fromStdString(stackId()) + ":second:static_text#0";
    auto* on_second = window.findChild<QWidget*>(derived);

    check(outside && stack && on_first && on_second, "every widget is built and named");
    if (!(outside && stack && on_first && on_second))
    {
        return;
    }

    check(agent_control::WidgetLocator::pathOf(outside) == "MainWindow/StaticTextWidget",
          "a top-level widget's path is unchanged by a stack beside it, got " +
              agent_control::WidgetLocator::pathOf(outside).toStdString());
    check(agent_control::WidgetLocator::pathOf(on_first).startsWith("MainWindow/PageStackWidget/PageStackPage"),
          "a page's widget is addressed through its stack and page, got " +
              agent_control::WidgetLocator::pathOf(on_first).toStdString());
    check(window.findChild<QWidget*>(QString::fromStdString(stackId()) + ":first") != nullptr,
          "a page is named <stack>:<page>");

    check(on_first->geometry() == QRect(10, 20, 120, 40), "a page's widget is placed relative to the stack");
    check(stack->geometry() == QRect(200, 100, 400, 300), "the stack is placed in the window");
    check(stack->page(0)->geometry() == QRect(0, 0, 400, 300), "a page fills its stack");

    check(dashboard::agent::configBearingWidget(stack->page(0)) == nullptr,
          "a selector landing on a page does not resolve to the page's first widget");
    check(dashboard::agent::configBearingWidget(stack) == stack, "the stack itself is configurable");

    check(stack->currentPage() == "second", "the default page is shown");
    check(on_second->isVisible() && !on_first->isVisible(), "only the current page's widgets are visible");

    const auto click = agent_control::sendClick(on_first, {});
    check(!click.has_value() && click.error().code == agent_control::ErrorCode::kWidgetNotVisible,
          "a click on a hidden page's widget is refused");

    check(stack->apply(page_action_t::go_to, "first"), "go_to applies");
    check(on_first->isVisible() && !on_second->isVisible(), "and swaps which widgets are visible");
    check(window.findChild<QWidget*>("on_first") == on_first, "without rebuilding anything");

    // Letterbox: the window grows, the layout is centred, and only top-level
    // widgets move -- a page's widgets ride along inside the stack.
    window.setMinimumSize(0, 0);
    window.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    window.resize(1000, 800);
    pump(50ms);
    check(stack->geometry() == QRect(300, 200, 400, 300), "the stack moves with the letterbox");
    check(on_first->geometry() == QRect(10, 20, 120, 40), "its widgets keep their stack-relative place");
}

void testRebuildsKeepPagesAndPlace()
{
    MainWindow window(windowConfig());
    window.show();
    pump(50ms);

    auto* stack = window.findChild<PageStackWidget*>(QString::fromStdString(stackId()));
    QPointer<QWidget> on_first = window.findChild<QWidget*>("on_first");
    if (!stack || !on_first)
    {
        check(false, "the window builds");
        return;
    }
    stack->apply(page_action_t::go_to, "first");

    // A widget on a page, as widget.set_config would rebuild it: type and
    // settings only, no id, no geometry.
    widget_config_t changed;
    changed.type = widget_type_t::static_text;
    StaticTextConfig_t text;
    text.text = "rebuilt";
    changed.config = text;
    check(window.rebuildWidget(on_first, changed), "a widget on a page can be rebuilt");
    pump(10ms);
    check(on_first.isNull(), "the old widget is gone");
    auto* replacement = qobject_cast<StaticTextWidget*>(window.findChild<QWidget*>("on_first"));
    check(replacement != nullptr, "the replacement keeps the name");
    if (replacement)
    {
        check(replacement->getConfig().text == "rebuilt", "with the new config");
        check(replacement->geometry() == QRect(10, 20, 120, 40), "in the same place on the page");
        check(replacement->isVisible(), "and visible, since its page is");
        check(stack->slotOf(replacement).has_value(), "and the stack knows it as its child");
    }

    // The stack itself, as set_config would: its pages are not in `config`.
    widget_config_t stack_change;
    stack_change.type = widget_type_t::page_stack;
    PageStackConfig_t stack_cfg;
    stack_cfg.default_page = "second";
    stack_change.config = stack_cfg;
    check(window.rebuildWidget(stack, stack_change), "the stack can be rebuilt");
    pump(10ms);
    auto* new_stack = window.findChild<PageStackWidget*>(QString::fromStdString(stackId()));
    check(new_stack != nullptr && new_stack->pageCount() == 2, "rebuilding the stack keeps its pages");
    check(new_stack != nullptr && new_stack->currentPage() == "first",
          "and stays on the page it was showing, not its new default");
    auto* rebuilt_child = qobject_cast<StaticTextWidget*>(window.findChild<QWidget*>("on_first"));
    check(rebuilt_child != nullptr && rebuilt_child->getConfig().text == "rebuilt",
          "and its pages carry the child edit made before it");
}

void testBrokenChildrenCount()
{
    app_config_t cfg = windowConfig();
    widget_page_t broken;
    broken.name = "broken";
    widget_config_t unknown;  // type unknown: nothing to build
    broken.widgets.push_back(unknown);
    widget_config_t nested;
    nested.type = widget_type_t::page_stack;
    nested.id = "nested";
    nested.config = PageStackConfig_t{};
    nested.pages = default_widget_pages(widget_type_t::page_stack);
    broken.widgets.push_back(nested);
    cfg.widgets[1].pages.push_back(broken);

    MainWindow window(cfg);
    check(window.widgetBuildFailures() == 2,
          "an unbuildable widget and a nested stack on a page both count as failures, got " +
              std::to_string(window.widgetBuildFailures()));
    check(window.findChild<PageStackWidget*>("nested") == nullptr, "a stack on a page is not built");
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testStructureNamesAndPlacement();
    testRebuildsKeepPagesAndPlace();
    testBrokenChildrenCount();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
