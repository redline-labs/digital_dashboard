cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

include_directories(
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/include
)

qt_add_executable(dashboard_editor
    src/main.cpp
    src/editor_window.cpp
    src/widget_palette.cpp
    src/palette_list.cpp
    src/canvas.cpp
    src/properties_panel.cpp
    src/selection_overlay.cpp
    resources.qrc
)

target_link_libraries(dashboard_editor PRIVATE
    Qt6::Core
    Qt6::Widgets

    # Reuse existing libraries and widgets
    common_logging
    helpers
    zenohcxx::zenohc

    # Widgets available for instantiation
    mercedes_190e_telltales
    carplay_widget
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

