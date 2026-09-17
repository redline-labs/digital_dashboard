// SPDX-License-Identifier: GPL-3.0-or-later
//
// PageNavigator: what each command does from each position. The cases worth
// pinning are the ones a UI gets subtly wrong -- a page outside the cycle, a
// go_to that erases where back would have gone, a cycle with one stop.

#include "page_stack/page_navigator.h"

#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using page_stack::PageNavigator;
using Result = PageNavigator::Result;

// carplay, vehicle, engine (in the cycle); diagnostics (not).
PageNavigator fourPages(std::string_view default_page = "")
{
    return PageNavigator({{"carplay", true}, {"vehicle", true}, {"diagnostics", false}, {"engine", true}},
                         default_page);
}

std::string currentName(const PageNavigator& nav)
{
    return nav.pages()[nav.current()].name;
}

void testDefaultPage()
{
    expect(currentName(fourPages()) == "carplay", "no default page starts on the first");
    expect(currentName(fourPages("engine")) == "engine", "a named default page is where it starts");
    expect(currentName(fourPages("nope")) == "carplay", "an unknown default falls back to the first");
    expect(!fourPages().previous().has_value(), "there is nothing to go back to at startup");
}

void testNextWrapsAndSkipsPagesOutOfTheCycle()
{
    auto nav = fourPages();
    expect(nav.apply(page_action_t::next, "") == Result::changed, "next changes the page");
    expect(currentName(nav) == "vehicle", "carplay -> vehicle");
    nav.apply(page_action_t::next, "");
    expect(currentName(nav) == "engine", "vehicle -> engine, skipping diagnostics");
    nav.apply(page_action_t::next, "");
    expect(currentName(nav) == "carplay", "engine wraps to carplay");
}

void testPrevWrapsAndSkipsPagesOutOfTheCycle()
{
    auto nav = fourPages();
    nav.apply(page_action_t::prev, "");
    expect(currentName(nav) == "engine", "prev from the first page wraps to the last");
    nav.apply(page_action_t::prev, "");
    expect(currentName(nav) == "vehicle", "engine -> vehicle, skipping diagnostics");
}

void testCyclingFromAPageOutsideTheCycle()
{
    // go_to diagnostics, then next: moves on to its neighbour, not to the start.
    auto nav = fourPages();
    nav.apply(page_action_t::go_to, "diagnostics");
    nav.apply(page_action_t::next, "");
    expect(currentName(nav) == "engine", "next from an out-of-cycle page goes to the page after it");

    nav.apply(page_action_t::go_to, "diagnostics");
    nav.apply(page_action_t::prev, "");
    expect(currentName(nav) == "vehicle", "prev from an out-of-cycle page goes to the page before it");
}

void testNothingInTheCycle()
{
    PageNavigator nav({{"a", false}, {"b", false}}, "a");
    expect(nav.apply(page_action_t::next, "") == Result::unchanged, "next with no cycle is unchanged");
    expect(nav.apply(page_action_t::prev, "") == Result::unchanged, "prev with no cycle is unchanged");
    expect(currentName(nav) == "a", "and the page stays");
    expect(nav.apply(page_action_t::go_to, "b") == Result::changed, "go_to still reaches any page");
}

void testOnePage()
{
    PageNavigator nav({{"only", true}}, "");
    expect(nav.apply(page_action_t::next, "") == Result::unchanged, "next with one page is unchanged");
    expect(!nav.previous().has_value(), "and records no previous page");
}

void testGoTo()
{
    auto nav = fourPages();
    expect(nav.apply(page_action_t::go_to, "engine") == Result::changed, "go_to a page changes to it");
    expect(currentName(nav) == "engine", "go_to engine shows engine");
    expect(nav.apply(page_action_t::go_to, "engine") == Result::unchanged, "go_to the current page is unchanged");
    expect(nav.previous() && *nav.previous() == 0, "and keeps where back would go");
    expect(nav.apply(page_action_t::go_to, "nope") == Result::unknown_page, "go_to an unknown page says so");
    expect(nav.apply(page_action_t::go_to, "") == Result::missing_page_name, "go_to with no name says so");
    expect(currentName(nav) == "engine", "neither failure moved the page");
}

void testBackToggles()
{
    auto nav = fourPages();
    expect(nav.apply(page_action_t::back, "") == Result::unchanged, "back with no history is unchanged");
    nav.apply(page_action_t::go_to, "vehicle");
    nav.apply(page_action_t::back, "");
    expect(currentName(nav) == "carplay", "back returns to the page before");
    nav.apply(page_action_t::back, "");
    expect(currentName(nav) == "vehicle", "back again toggles to where back came from");
}

void testEmpty()
{
    PageNavigator nav({}, "");
    expect(nav.empty(), "no pages is empty");
    expect(nav.apply(page_action_t::next, "") == Result::empty, "every command on no pages says empty");
    expect(nav.apply(page_action_t::go_to, "a") == Result::empty, "go_to on no pages says empty");
}

}  // namespace

int main()
{
    testDefaultPage();
    testNextWrapsAndSkipsPagesOutOfTheCycle();
    testPrevWrapsAndSkipsPagesOutOfTheCycle();
    testCyclingFromAPageOutsideTheCycle();
    testNothingInTheCycle();
    testOnePage();
    testGoTo();
    testBackToggles();
    testEmpty();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
