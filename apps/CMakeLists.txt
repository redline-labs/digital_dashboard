# SPDX-License-Identifier: GPL-3.0-or-later
#
# The GUI applications. Each is a sibling: they share libs/ (config_codec,
# qt_helpers, agent_control, dashboard_widgets, map_render, ...) by target
# name and never include from one another's tree. That rule is what let scope
# be added without touching the dashboard, and it is why the editor is a
# directory here rather than a second executable inside dashboard/.
add_subdirectory(dashboard)
add_subdirectory(editor)
add_subdirectory(scope)
add_subdirectory(switchboard)
