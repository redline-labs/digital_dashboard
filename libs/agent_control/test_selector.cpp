// Selector resolution against a real widget tree.
//
// The property under test is not "does a good selector work" but "does a bad or
// under-specified selector fail loudly". Silently resolving an ambiguous
// selector to its first match is the failure that makes an agent drive the wrong
// widget and then report a confident, wrong conclusion.

#include "agent_control/locator.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

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

std::string reasonOf(const agent_control::AgentError& error)
{
    return std::string(agent_control::toString(error.code));
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // A window with: one named button, three unnamed sibling labels, and a
    // nested child. Enough to exercise names, classes, indices and paths.
    QWidget window;
    window.setObjectName("root_window");
    window.resize(400, 300);

    auto* named = new QPushButton("Go", &window);
    named->setObjectName("go_button");
    named->setGeometry(10, 10, 80, 30);

    for (int i = 0; i < 3; ++i)
    {
        auto* label = new QLabel(QString("label %1").arg(i), &window);
        label->setGeometry(10, 50 + 30 * i, 100, 20);
    }

    auto* container = new QWidget(&window);
    container->setObjectName("container");
    container->setGeometry(150, 10, 200, 200);
    auto* nested = new QPushButton("Nested", container);
    nested->setObjectName("nested_button");

    window.show();

    agent_control::WidgetLocator locator;
    locator.setRoots({&window});

    // ------------------------------------------------------------ by object name
    {
        auto r = locator.resolve("#go_button");
        check(r.has_value() && r.value() == named, "#id resolves to the named widget");
    }
    {
        auto r = locator.resolve("go_button");
        check(r.has_value() && r.value() == named, "a bare name resolves like #id");
    }
    {
        auto r = locator.resolve("#nested_button");
        check(r.has_value() && r.value() == nested, "#id finds a deeply nested widget");
    }

    // ---------------------------------------------------------------- ambiguity
    {
        // Three QLabels with no names. This MUST fail rather than pick one.
        auto r = locator.resolve("QLabel");
        check(!r.has_value(), "a class matching several widgets does not resolve");
        check(!r.has_value() && reasonOf(r.error()) == "AMBIGUOUS_SELECTOR",
              "several matches reports AMBIGUOUS_SELECTOR");
        check(!r.has_value() && r.error().data["matches"].size() == 3,
              "the error lists every candidate so the caller can disambiguate");
    }
    {
        // QPushButton matches both the top-level and the nested one.
        auto r = locator.resolve("QPushButton");
        check(!r.has_value() && reasonOf(r.error()) == "AMBIGUOUS_SELECTOR",
              "ambiguity is detected across nesting levels too");
    }

    // --------------------------------------------------------------- not found
    {
        auto r = locator.resolve("#no_such_thing");
        check(!r.has_value() && reasonOf(r.error()) == "NO_SUCH_WIDGET",
              "an unknown id reports NO_SUCH_WIDGET");
        check(!r.has_value() && !r.error().data["candidates"].empty(),
              "an unknown id offers the names that do exist");
    }
    {
        auto r = locator.resolve("");
        check(!r.has_value() && reasonOf(r.error()) == "BAD_PARAMS",
              "an empty selector is rejected");
    }

    // -------------------------------------------------------------- by path
    {
        // Each path segment is a strict parent->child step, so this matches only
        // the button parented directly to the window -- the nested one is a
        // grandchild and does not collide. That strictness is the point: it is
        // what lets a path disambiguate a class name that is ambiguous on its
        // own (QPushButton alone matches both, as asserted above).
        auto r = locator.resolve("QWidget/QPushButton");
        check(r.has_value() && r.value() == named,
              "a path segment matches direct children only, so nesting does not collide");
    }
    {
        auto r = locator.resolve("QWidget/container/nested_button");
        check(r.has_value() && r.value() == nested, "a full path resolves");
    }
    {
        auto r = locator.resolve("QWidget/QLabel[1]");
        check(r.has_value(), "an index disambiguates identical siblings");
        check(r.has_value() && qobject_cast<QLabel*>(r.value()) != nullptr,
              "the indexed match is the right class");
    }
    {
        auto r = locator.resolve("QWidget/QLabel[9]");
        check(!r.has_value() && reasonOf(r.error()) == "NO_SUCH_WIDGET",
              "an out-of-range index is NO_SUCH_WIDGET, not a clamp");
    }

    // ------------------------------------------------------------------ paths
    {
        // The [n] suffix must appear only where it disambiguates, otherwise
        // every path would carry noise and change whenever a sibling appears.
        const QString button_path = agent_control::WidgetLocator::pathOf(named);
        check(!button_path.contains('['), "a unique sibling gets no [n] suffix");
        check(button_path == "QWidget/QPushButton", "path is class-based from the window");

        const QString nested_path = agent_control::WidgetLocator::pathOf(nested);
        check(nested_path == "QWidget/QWidget/QPushButton", "nested paths include every level");
    }

    // ------------------------------------------------------------------- refs
    {
        const QString ref = locator.refFor(named);
        check(!ref.isEmpty(), "a ref is issued");
        check(locator.refFor(named) == ref, "asking twice returns the same ref");

        auto r = locator.resolve(ref.toStdString());
        check(r.has_value() && r.value() == named, "a ref resolves back to its widget");
    }
    {
        auto r = locator.resolve("w9999");
        check(!r.has_value() && reasonOf(r.error()) == "STALE_REF",
              "a ref that was never issued is STALE_REF");
    }
    {
        // A destroyed widget must report staleness rather than resolving to
        // freed memory -- this is what the QPointer in the ref table is for.
        auto* temporary = new QPushButton("temp", &window);
        temporary->setObjectName("temp_button");
        const QString ref = locator.refFor(temporary);
        delete temporary;

        auto r = locator.resolve(ref.toStdString());
        check(!r.has_value() && reasonOf(r.error()) == "STALE_REF",
              "a ref to a destroyed widget is STALE_REF, not a dangling pointer");
    }

    // -------------------------------------------------------------- revision
    {
        const std::uint64_t before = locator.revision();
        locator.noteTreeState(locator.allWidgets());
        locator.noteTreeState(locator.allWidgets());
        const std::uint64_t stable = locator.revision();

        auto* fresh = new QPushButton("fresh", &window);
        fresh->setObjectName("fresh_button");
        locator.noteTreeState(locator.allWidgets());

        check(locator.revision() != stable, "adding a widget bumps the revision");
        check(before <= stable, "revision never goes backwards");

        // Re-observing an unchanged tree must not bump, or the revision would be
        // useless as a "has anything moved?" signal.
        const std::uint64_t after_add = locator.revision();
        locator.noteTreeState(locator.allWidgets());
        check(locator.revision() == after_add, "an unchanged tree leaves the revision alone");
    }

    std::printf("%s: %d checks, %d failures\n", argv[0], g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
