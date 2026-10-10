// SPDX-License-Identifier: GPL-3.0-or-later
//
// forEachPageCommand finds a page command wherever a config puts one: a direct
// field, a nested struct, a list of structs. The editor's page rename relies on
// it to reach every widget that targets a stack, without naming them.

#include "dashboard/page_command_walk.h"

#include "config_codec/config_yaml.h"

#include <cstdio>
#include <string>
#include <vector>

REFLECT_STRUCT(walk_inner_t,
    (std::string, label, ""),
    (page_command_t, command, page_command_t{})
)

REFLECT_STRUCT(walk_outer_t,
    (page_command_t, direct, page_command_t{}),
    (walk_inner_t, nested, walk_inner_t{}),
    (std::vector<walk_inner_t>, buttons, {}),
    (int, unrelated, 0)
)

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main()
{
    walk_outer_t config;
    config.direct.page = "a";
    config.nested.command.page = "b";
    config.buttons.resize(2);
    config.buttons[0].command.page = "c";
    config.buttons[1].command.page = "d";

    std::string seen;
    dashboard::forEachPageCommand(config, [&](page_command_t& command) { seen += command.page; });
    expect(seen == "abcd", "every command is found, in field order (got '" + seen + "')");

    dashboard::forEachPageCommand(config, [](page_command_t& command) { command.page = "renamed"; });
    expect(config.buttons[1].command.page == "renamed", "and found by reference, so a rename sticks");

    // Through a real widget, the way the editor calls it.
    widget_config_t widget;
    widget.config = default_widget_config(widget_type_t::page_button);
    int count = 0;
    dashboard::forEachPageCommand(widget, [&](page_command_t&) { ++count; });
    expect(count == 1, "a page_button has one command");

    widget.config = default_widget_config(widget_type_t::carplay);
    count = 0;
    dashboard::forEachPageCommand(widget, [&](page_command_t&) { ++count; });
    expect(count == 1, "CarPlay's return button is found without being named");

    widget.config = default_widget_config(widget_type_t::static_text);
    count = 0;
    dashboard::forEachPageCommand(widget, [&](page_command_t&) { ++count; });
    expect(count == 0, "a widget without one has none");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
