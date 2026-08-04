# Every Qt component any widget links, found once here rather than per widget.
# find_package creates the Qt6:: imported targets in the calling directory scope
# only, so a component found down in widgets/ does not exist up here, where the
# executables are finalized: Qt's dependency walk then warns that the target "is
# mentioned as a dependency ... but not declared". Finding them before
# add_subdirectory() also lets the widgets inherit the targets, so their own
# find_package calls become no-ops instead of re-running Qt's dependency probes.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Multimedia)

add_subdirectory(widgets)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

qt_standard_project_setup()


include_directories(
    include
)

# Widget libraries linked by both the dashboard and the editor. Add new
# widgets here (and in FOR_EACH_WIDGET / widget_types.h / YAML_CONFIG_STRUCT).
set(WIDGET_LIBS
    carplay_widget
    now_playing
    mercedes_190e_telltales
    mercedes_190e_speedometer
    mercedes_190e_tachometer
    sparkline
    mercedes_190e_cluster_gauge
    motec_c125_tachometer
    motec_cdl3_tachometer
    static_text
    value_readout
    background_rect
)

qt_add_executable(${PROJECT_NAME}
    dashboard/main.cpp
    dashboard/app_config.cpp
    dashboard/main_window.cpp
    dashboard/command_line_args.cpp

    # Shared by both executables: the widget config methods need FOR_EACH_WIDGET,
    # so they cannot live in libs/agent_control (which knows nothing about
    # widgets and must not).
    agent/widget_methods.cpp

    resources.qrc
)

#set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets

    # Agent control interface (--mcp). Always built in; only listens when the
    # flag is supplied.
    agent_control

    # Link common logging interface (wraps spdlog)
    zenoh_pub_sub
    helpers
    
    # Link spdlog
    spdlog::spdlog

    # Link nlohmann/json (now fetched via FetchContent)
    nlohmann_json::nlohmann_json
    
    # Link cxxopts (now fetched via FetchContent)
    cxxopts::cxxopts
    
    # Link yaml-cpp (now fetched via FetchContent)
    yaml-cpp::yaml-cpp

    ${WIDGET_LIBS}
)

qt_add_executable(editor
    editor/main.cpp
    editor/editor_window.cpp
    editor/widget_palette.cpp
    editor/canvas.cpp
    editor/properties_panel.cpp
    editor/selection_frame.cpp
    editor/editor_methods.cpp

    dashboard/app_config.cpp  # TODO, kinda hack.
    agent/widget_methods.cpp

    resources.qrc
)

# macOS: ensure the app bundle displays the desired name in the menu bar
if (APPLE)
    #set(DEFAULT_EDITOR_ICON "${CMAKE_SOURCE_DIR}/dashboard/resources/icons/editor.icns")

    # Set the bundle icon name (filename only)
    set_target_properties(editor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Dashboard Editor"
        #MACOSX_BUNDLE_ICON_FILE "editor.icns"
    )
    # Add the .icns to the target and place it in the app bundle Resources
    #target_sources(editor PRIVATE "${DEFAULT_EDITOR_ICON}")
    #set_source_files_properties("${DEFAULT_EDITOR_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

# ---------------------------------------------------------------------- tests

# The YAML codec and the validator are plain logic over reflected structs, so
# they are unit-testable without a display. Both link the widget libraries
# because their coverage is driven by FOR_EACH_WIDGET.
foreach(config_test roundtrip validation)
    add_executable(dashboard_test_config_${config_test} dashboard/test_config_${config_test}.cpp)
    target_include_directories(dashboard_test_config_${config_test} PRIVATE include)
    target_sources(dashboard_test_config_${config_test} PRIVATE dashboard/app_config.cpp)
    target_link_libraries(dashboard_test_config_${config_test} PRIVATE
        Qt6::Core
        Qt6::Widgets
        zenoh_pub_sub
        helpers
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        ${WIDGET_LIBS}
    )
    add_project_test(TARGET dashboard_test_config_${config_test} LABELS dashboard unit)
endforeach()

# The editor's own load/edit/export path, above the codec. Constructs real
# widgets, so it is a gui test and runs offscreen.
qt_add_executable(editor_test_roundtrip
    editor/test_editor_roundtrip.cpp
    editor/canvas.cpp
    editor/properties_panel.cpp
    editor/selection_frame.cpp
    dashboard/app_config.cpp
)
target_include_directories(editor_test_roundtrip PRIVATE include)
target_link_libraries(editor_test_roundtrip PRIVATE
    Qt6::Core
    Qt6::Widgets
    zenoh_pub_sub
    helpers
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    ${WIDGET_LIBS}
)
add_project_test(TARGET editor_test_roundtrip LABELS dashboard gui)

target_link_libraries(editor PRIVATE
    Qt6::Core
    Qt6::Widgets

    # Agent control interface (--mcp), same as the dashboard.
    agent_control

    # Reuse existing libraries and widgets
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    cxxopts::cxxopts
    zenoh_pub_sub
    helpers

    ${WIDGET_LIBS}
)
