# SPDX-License-Identifier: GPL-3.0-or-later
#
# dashboard -- the instrument cluster.
#
# Renders a layout: a YAML file naming windows, the widgets in each, and the
# bus expressions that drive them. The widgets and the layout config are
# libs/dashboard_widgets, shared with the editor; what is here is only what
# makes a running cluster out of one -- the window, the display binding, the
# config override on the target, and the command line.

# Repeated here rather than inherited: find_package creates the Qt6:: imported
# targets in the calling directory scope only. Same as apps/scope/CMakeLists.txt.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
qt_standard_project_setup()

qt_add_executable(dashboard
    main.cpp
    main_window.cpp
    display_binding.cpp
    config_override.cpp
    command_line_args.cpp

    # Q_OBJECT header, listed so AUTOMOC finds it under include/. See the note
    # in apps/scope/CMakeLists.txt.
    include/dashboard/main_window.h
)

target_include_directories(dashboard PRIVATE include)

target_link_libraries(dashboard PRIVATE
    core
    Qt6::Core
    Qt6::Widgets

    # The widgets, the layout config and its loader, the widget.* agent methods.
    dashboard_widgets

    # Agent control interface (--mcp). Always built in; only listens when the
    # flag is supplied.
    agent_control

    zenoh_pub_sub
    helpers
    config_codec
    qt_helpers

    spdlog::spdlog
    nlohmann_json::nlohmann_json
    cxxopts::cxxopts
    yaml-cpp::yaml-cpp
)

# ---------------------------------------------------------------------- tests

# Which display a window binds to and at what scale, against a fake environment.
# Pure C++: no widgets, no Qt, no display.
add_executable(dashboard_test_display_binding
    tests/test_display_binding.cpp
    display_binding.cpp
)
# display_binding.h names window_placement.h from the shared include tree, and
# that is all it wants from there: the header, not the widgets behind the
# library, which would drag Qt into a test that has none.
target_include_directories(dashboard_test_display_binding PRIVATE
    include
    ${DASHBOARD_WIDGET_INCLUDE_DIR}
)
target_link_libraries(dashboard_test_display_binding PRIVATE reflection)
add_project_test(TARGET dashboard_test_display_binding LABELS dashboard unit)

# Which config runs -- shipped, or an operator's override on the data partition
# -- and the attempt marker that keeps a crashing override from taking both
# slots down. Links the loader (so the real one is exercised) but needs no
# display.
add_executable(dashboard_test_config_override
    tests/test_config_override.cpp
    config_override.cpp
)
target_include_directories(dashboard_test_config_override PRIVATE include)
target_link_libraries(dashboard_test_config_override PRIVATE
    dashboard_widgets
    spdlog::spdlog
)
add_project_test(TARGET dashboard_test_config_override LABELS dashboard unit)
