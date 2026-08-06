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
    decimate.cpp
    live_zenoh_source.cpp
    time_base.cpp
    panel_registry.cpp
    signal_browser.cpp
    add_signal_dialog.cpp
    scope_window.cpp
    workspace.cpp
    scope_methods.cpp
    command_line_args.cpp

    # Headers carrying Q_OBJECT have to be listed as sources, because AUTOMOC
    # only looks for a matching header next to the .cpp that includes the moc
    # file -- and ours live under include/scope/. Leaving one out fails either
    # as "could not be found in the following directories" at generate time or,
    # worse, as a missing staticMetaObject at link time.
    include/scope/scope_window.h
    include/scope/time_base.h
    include/scope/panel.h
    include/scope/signal_browser.h
    include/scope/add_signal_dialog.h

    # Panels. One directory per type, mirroring dashboard/widgets/ -- but built
    # into scope_core rather than into a library each, because there is no
    # equivalent of the widget palette's dynamic registration to serve and the
    # panel table is resolved at compile time anyway.
    panels/time_series/time_series_panel.cpp
    panels/time_series/include/time_series/time_series_panel.h
)

set_target_properties(scope_core PROPERTIES AUTOMOC ON)

target_include_directories(scope_core PUBLIC
    include
    panels/time_series/include
)

target_link_libraries(scope_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

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

# Per-column min/max reduction, against a brute-force reference over random
# data. Runs on every signal on every frame, and every way it can be wrong looks
# like data rather than like a bug.
add_executable(scope_test_decimate
    tests/test_decimate.cpp
)

target_link_libraries(scope_test_decimate PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_decimate LABELS scope unit)

# The workspace codec, including a byte-stability round trip over every shipped
# workspace. A codec that round-trips semantically but reorders keys turns every
# save into a diff, which makes a workspace impossible to keep in version
# control -- and that is exactly what someone will want to do with one.
add_executable(scope_test_workspace
    tests/test_workspace.cpp
)

# The shipped workspaces are the corpus, so the test has to find them wherever
# ctest happens to be run from. Same trick as editor_test_roundtrip.
target_compile_definitions(scope_test_workspace PRIVATE
    SCOPE_CONFIG_DIR="${CMAKE_SOURCE_DIR}/configs/scope")

target_link_libraries(scope_test_workspace PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_workspace LABELS scope unit)

# The window and panel layer against a real widget tree, so `gui` and offscreen.
# Uses a stub DataSource rather than the live zenoh one: everything here is
# widget behaviour, and needing a bus would make it a `net` test that skips
# itself on a machine without one.
qt_add_executable(scope_test_panels
    tests/test_panels.cpp
)

target_link_libraries(scope_test_panels PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_panels LABELS scope gui)
