# SPDX-License-Identifier: GPL-3.0-or-later

# Small Qt helpers shared by the GUI applications: a layered paint-caching
# QWidget base, helpers::Color -> QColor, resource font loading, and quitting
# on SIGINT/SIGTERM.
#
# The Qt-using sibling of `helpers`, which is deliberately Qt-free. These were
# in dashboard/include/dashboard/ and nothing in them names a dashboard; the
# scope app draws its plots on CachedPaintWidget and resolves colours the same
# way.
#
# find_package is repeated here because imported targets are directory-scoped --
# the same reason libs/agent_control/CMakeLists.txt has to do it.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Static rather than header-only for one file: quitOnSignals keeps a handler
# and a pipe that must exist once per process.
add_library(qt_helpers STATIC
    quit_on_signal.cpp
)

target_include_directories(qt_helpers PUBLIC
    include
)

target_link_libraries(qt_helpers PUBLIC
    helpers
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    spdlog::spdlog
)

# SIGTERM -- what systemd sends -- and SIGINT both end the event loop, and a
# second signal is not caught. Needs an event loop, not a display.
add_executable(qt_helpers_test_quit_on_signal tests/test_quit_on_signal.cpp)
target_link_libraries(qt_helpers_test_quit_on_signal PRIVATE qt_helpers)
add_project_test(TARGET qt_helpers_test_quit_on_signal LABELS qt_helpers unit)
