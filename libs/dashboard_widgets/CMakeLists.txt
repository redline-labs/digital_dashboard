# SPDX-License-Identifier: GPL-3.0-or-later
#
# dashboard_widgets -- the widget set, and what it takes to build a layout out
# of it.
#
# Two applications consume the same widgets: the dashboard renders a layout,
# the editor edits one. Everything they need in common lives here, so that
# neither includes from the other's tree: the widget table and the registry
# generated from it, the reflected layout config (app_config.h) and its loader,
# the factory that turns a config into a QWidget, and the widget.* agent
# methods, which sweep the widget table and so cannot live in libs/agent_control
# (which knows nothing about widgets and must not).
#
# The include prefix stays `dashboard/` -- these headers describe a dashboard
# layout whoever is asking, and the moc-at-end-of-cpp form in the apps resolves
# through it.

# Repeated here rather than inherited: find_package creates the Qt6:: imported
# targets in the calling directory scope only. Same as apps/scope/CMakeLists.txt.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Multimedia)

set(CMAKE_AUTOMOC ON)
qt_standard_project_setup()

# Every widget is its own static library and registers itself; see
# cmake/DashboardWidget.cmake. The widgets find this directory's include/ through
# DASHBOARD_WIDGET_INCLUDE_DIR, which that module computes, so they do not link
# this target -- they cannot, it links them.
add_subdirectory(widgets)

# Collected from the widgets themselves: add_dashboard_widget() appends to this
# property, so a widget is named once, in its own CMakeLists, rather than there
# and again in a list up here that nothing checks. Read after
# add_subdirectory(widgets) above, by which point every widget has registered.
get_property(WIDGET_LIBS GLOBAL PROPERTY DASHBOARD_WIDGET_LIBS)

if(NOT WIDGET_LIBS)
    message(FATAL_ERROR
        "No widgets registered. Every widget's CMakeLists should call "
        "add_dashboard_widget() or dashboard_widget_register().")
endif()

add_library(dashboard_widgets STATIC
    app_config.cpp
    widget_methods.cpp
)

# The fonts and SVGs the widgets load from ":/". Added through qt_add_resources
# rather than by listing a .qrc as a source: a .qrc compiled INTO a static
# library is dropped at link time, because nothing references its initializer,
# and the failure is a wrong font and a missing telltale rather than an error.
# qt_add_resources puts the resource in an object library that Qt propagates to
# whatever links this target, so both apps get it without naming it.
set(DASHBOARD_WIDGET_RESOURCES
    resources/fonts/futura.ttf
    resources/fonts/DSEG/DSEG7ClassicMini-Bold.ttf
    resources/fonts/DSEG/DSEG7Classic-Bold.ttf
    resources/fonts/DSEG/DSEG14Classic-Regular.ttf
    resources/widgets/mercedes_190e_telltales/telltale_windshield_washer.svg
    resources/widgets/mercedes_190e_telltales/telltale_brake_system.svg
    resources/widgets/mercedes_190e_telltales/telltale_battery.svg
    resources/widgets/mercedes_190e_telltales/telltale_high_beam.svg
    resources/widgets/mercedes_190e_cluster_gauge/coolant_icon.svg
    resources/widgets/mercedes_190e_cluster_gauge/gas_icon.svg
    resources/widgets/mercedes_190e_cluster_gauge/oil_icon.svg
)
# The paths the widgets ask for are flatter than the tree: ":/fonts/<file>" and
# ":/<widget>/<file>".
foreach(res IN LISTS DASHBOARD_WIDGET_RESOURCES)
    get_filename_component(res_name "${res}" NAME)
    get_filename_component(res_dir "${res}" DIRECTORY)
    if(res_dir MATCHES "^resources/fonts")
        set_source_files_properties(${res} PROPERTIES QT_RESOURCE_ALIAS "fonts/${res_name}")
    else()
        get_filename_component(widget_name "${res_dir}" NAME)
        set_source_files_properties(${res} PROPERTIES QT_RESOURCE_ALIAS "${widget_name}/${res_name}")
    endif()
endforeach()
qt_add_resources(dashboard_widgets dashboard_widget_resources
    PREFIX "/"
    FILES ${DASHBOARD_WIDGET_RESOURCES}
)

target_include_directories(dashboard_widgets PUBLIC include)

target_link_libraries(dashboard_widgets PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

    ${WIDGET_LIBS}

    reflection
    helpers
    config_codec
    qt_helpers
    zenoh_pub_sub
    agent_control

    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
  PRIVATE
    spdlog::spdlog
)

# ---------------------------------------------------------------------- tests

# The YAML codec and the validator are plain logic over reflected structs, so
# they are unit-testable without a display. Their coverage is driven by
# DASHBOARD_WIDGET_TABLE, which is why they live with the widgets rather than
# with either app.
# The loss-of-comm state machine. Pure chrono arithmetic over an injected
# clock, so the boundaries can be pinned exactly rather than waited for.
add_executable(dashboard_widgets_test_staleness tests/test_staleness.cpp)
target_link_libraries(dashboard_widgets_test_staleness PRIVATE
    dashboard_widgets
)
add_project_test(TARGET dashboard_widgets_test_staleness LABELS dashboard_widgets unit)

# Loss of comm, over the whole widget table: a widget that reads a topic has to
# implement StaleAware, and going stale has to change what it draws. A stale
# look that renders identically is the failure this test exists for.
add_executable(dashboard_widgets_test_stale_render tests/test_stale_render.cpp)
target_link_libraries(dashboard_widgets_test_stale_render PRIVATE
    dashboard_widgets
    spdlog::spdlog
)
add_project_test(TARGET dashboard_widgets_test_stale_render LABELS dashboard_widgets gui)

# The hook against a real publisher: no flicker on a live stream, stale once it
# stops, fresh again when it resumes, and silent under the editor's suppression.
add_executable(dashboard_widgets_test_subscription_stale tests/test_subscription_stale.cpp)
target_link_libraries(dashboard_widgets_test_subscription_stale PRIVATE
    dashboard_widgets
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET dashboard_widgets_test_subscription_stale
                 LABELS dashboard_widgets net gui)

foreach(config_test roundtrip validation colors)
    add_executable(dashboard_widgets_test_config_${config_test}
        tests/test_config_${config_test}.cpp)
    target_link_libraries(dashboard_widgets_test_config_${config_test} PRIVATE
        dashboard_widgets
        spdlog::spdlog
    )
    add_project_test(TARGET dashboard_widgets_test_config_${config_test}
                     LABELS dashboard_widgets unit)
endforeach()
