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
# Qt itself is found once, in the root CMakeLists, before libs/ is added.

# Static rather than header-only: quitOnSignals keeps a handler and a pipe that
# must exist once per process, and ReflectedForm is a QObject that needs moc.
add_library(qt_helpers STATIC
    quit_on_signal.cpp
    # The form both GUI apps edit reflected configs with; a QObject, so moc'd.
    reflected_form.cpp
    include/qt_helpers/reflected_form.h
)
set_target_properties(qt_helpers PROPERTIES AUTOMOC ON)

target_include_directories(qt_helpers PUBLIC
    include
)

target_link_libraries(qt_helpers PUBLIC
    helpers
    reflection
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

# The reflected-config form: every field kind writes through, no integer the
# editors accept can wrap, ruled fields report their problems, lists rebuild.
add_executable(qt_helpers_test_reflected_form tests/test_reflected_form.cpp)
target_link_libraries(qt_helpers_test_reflected_form PRIVATE qt_helpers)
add_project_test(TARGET qt_helpers_test_reflected_form LABELS qt_helpers gui)
