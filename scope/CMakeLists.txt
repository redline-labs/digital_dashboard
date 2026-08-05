# SPDX-License-Identifier: GPL-3.0-or-later
#
# scope -- the live time-series visualizer.
#
# A sibling of dashboard/, not a subdirectory of it. It shares the reflected
# config machinery (config_codec) and the Qt widget helpers (qt_helpers) with
# the dashboard and the editor, but nothing else: dashboard widgets are not
# scope panels, so ${WIDGET_LIBS} is deliberately absent from the link line.

# Repeated here rather than inherited: find_package creates the Qt6:: imported
# targets in the calling directory scope only. Same reason
# libs/agent_control/CMakeLists.txt has to do it.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
qt_standard_project_setup()

qt_add_executable(scope
    main.cpp
    command_line_args.cpp
    scope_window.cpp

    # Headers carrying Q_OBJECT have to be listed as sources, because AUTOMOC
    # only looks for a matching header next to the .cpp that includes the moc
    # file -- and ours live under include/scope/. Leaving one out fails either
    # as "could not be found in the following directories" at generate time or,
    # worse, as a missing staticMetaObject at link time.
    include/scope/scope_window.h
)

target_include_directories(scope PRIVATE
    include
)

target_link_libraries(scope PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

    # Agent control interface (--mcp). Always built in; only listens when the
    # flag is supplied.
    agent_control

    zenoh_pub_sub
    config_codec
    qt_helpers
    reflection
    helpers

    spdlog::spdlog
    yaml-cpp::yaml-cpp
    cxxopts::cxxopts
    nlohmann_json::nlohmann_json
)
