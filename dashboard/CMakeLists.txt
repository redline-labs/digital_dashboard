add_subdirectory(widgets)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()


include_directories(
    include
)

qt_add_executable(${PROJECT_NAME}
    dashboard/main.cpp
    dashboard/app_config.cpp
    dashboard/main_window.cpp
    dashboard/command_line_args.cpp
    resources.qrc
)

#set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    
    # Link common logging interface (wraps spdlog)
    common_logging
    zenoh_pub_sub
    helpers
    
    # Link nlohmann/json (now fetched via FetchContent)
    nlohmann_json::nlohmann_json
    
    # Link cxxopts (now fetched via FetchContent)
    cxxopts::cxxopts
    
    # Link yaml-cpp (now fetched via FetchContent)
    yaml-cpp::yaml-cpp

    # Link the carplay widget library
    #carplay_widget
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

qt_add_executable(editor
    editor/main.cpp
    editor/editor_window.cpp
    editor/widget_palette.cpp
    editor/canvas.cpp
    editor/properties_panel.cpp
    editor/selection_frame.cpp

    dashboard/app_config.cpp  # TODO, kinda hack.

    resources.qrc
)

target_link_libraries(editor PRIVATE
    Qt6::Core
    Qt6::Widgets

    # Reuse existing libraries and widgets
    common_logging
    yaml-cpp::yaml-cpp
    zenoh_pub_sub
    helpers

    # Widgets available for instantiation
    mercedes_190e_telltales
    #carplay_widget
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
