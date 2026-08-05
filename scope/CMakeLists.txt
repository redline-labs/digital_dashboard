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

# Everything except main(), so tests can link it. A test that had to link the
# executable would need main() out of the way, and the usual workarounds for
# that (a -DTESTING switch, a weak symbol) are worse than a library.
add_library(scope_core STATIC
    sample_ring.cpp
    live_zenoh_source.cpp
    time_base.cpp
    scope_window.cpp
    command_line_args.cpp

    # Headers carrying Q_OBJECT have to be listed as sources, because AUTOMOC
    # only looks for a matching header next to the .cpp that includes the moc
    # file -- and ours live under include/scope/. Leaving one out fails either
    # as "could not be found in the following directories" at generate time or,
    # worse, as a missing staticMetaObject at link time.
    include/scope/scope_window.h
    include/scope/time_base.h
)

set_target_properties(scope_core PROPERTIES AUTOMOC ON)

target_include_directories(scope_core PUBLIC
    include
)

target_link_libraries(scope_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

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

qt_add_executable(scope
    main.cpp
)

target_link_libraries(scope PRIVATE
    scope_core

    # Agent control interface (--mcp). Always built in; only listens when the
    # flag is supplied.
    agent_control
)

# ---------------------------------------------------------------------- tests

# The buffering layer on its own: the lock-free hand-off from the producer
# thread, the retained history, and the retention limits. No zenoh and no Qt, so
# this is a plain unit test and runs everywhere.
add_executable(scope_test_ring
    tests/test_sample_ring.cpp
)

target_link_libraries(scope_test_ring PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_ring LABELS scope unit)
