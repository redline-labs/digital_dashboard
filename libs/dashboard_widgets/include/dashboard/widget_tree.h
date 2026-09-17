// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building a configured widget, and whatever it contains.
#ifndef DASHBOARD_WIDGET_TREE_H_
#define DASHBOARD_WIDGET_TREE_H_

#include "dashboard/app_config.h"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <string>

namespace dashboard
{

struct BuildResult
{
    // Null when the widget itself could not be built.
    QWidget* widget = nullptr;
    // Widgets on its pages that could not be built. The stack is still usable,
    // but the layout is missing something it asked for.
    std::size_t child_failures = 0;
};

// Builds one configured widget, named `object_name`, as a child of `parent`.
// Geometry is the caller's.
//
// For a page_stack this also builds every page and every widget on them, then
// starts the stack -- which is what subscribes it to its topics. `keep_page`
// shows that page instead of the default, for a rebuild that should not jump.
// A page_stack on a page is refused and counted as a failure: validation
// rejects it, and one level is all the stack is built for.
//
// The one place both apps build widgets that can contain widgets, so the
// dashboard and (later) the editor agree on names and structure.
BuildResult buildWidget(const widget_config_t& cfg, const QString& object_name, QWidget* parent,
                        std::optional<std::string> keep_page = std::nullopt);

// Builds a widget that sits on a page, placed and shown. Null on failure.
QWidget* buildPageChild(const widget_config_t& cfg, const QString& object_name, QWidget* page);

}  // namespace dashboard

#endif  // DASHBOARD_WIDGET_TREE_H_
