# Offline Map Widget
#
# What is left here after the renderer was hoisted into libs/map_render: the
# QWidget itself, its reflected config, and the zenoh client that asks
# nodes/map_server for tiles. Everything that turns a vector tile into pixels
# is in the library and is shared with scope's map panel.
#
# The split is along "does this know where tiles come from". TileSource does --
# it holds a ZenohAsyncClient -- so it stays. The tessellator and the GPU
# renderer do not, so they went.
cmake_minimum_required(VERSION 3.16)

set(MAP_WIDGET_LIB map_widget)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(${MAP_WIDGET_LIB} STATIC
    map_widget.cpp
    tile_source.cpp
    include/map/config.h
    include/map/highlight_ids.h
    include/map/map_widget.h
    include/map/tile_source.h
)

target_include_directories(${MAP_WIDGET_LIB} PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    include
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(${MAP_WIDGET_LIB}
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        # The renderer. PUBLIC because map_widget.h and tile_source.h both
        # expose its types -- Projection, TileId, CachedTile, GpuRenderer.
        map_render
        # The floating buttons. PUBLIC because map_widget.h holds pointers to
        # its widget types.
        map_controls
        helpers
        qt_helpers
        reflection
        config_codec
        schemas
        zenoh_pub_sub
        mvt
    PRIVATE
        # Only for road_graph/format.h's constexpr wayOf(): the highlight joins
        # horizon segment ids back to the way ids stamped on tile features.
        road_graph
        spdlog::spdlog
)

# Registered by hand rather than through add_dashboard_widget(), the way the
# carplay widget is: this one links map_render, mvt and road_graph beyond the
# shared defaults.
dashboard_widget_register(${MAP_WIDGET_LIB})

set_target_properties(${MAP_WIDGET_LIB} PROPERTIES AUTOMOC ON)

# ------------------------------------------------------------------- tests

# The widget through the paths the apps and the agent interface use: the config
# surface, and that it paints offscreen.
add_executable(map_test_widget
    test_map_widget.cpp
)

target_link_libraries(map_test_widget
    PRIVATE
        ${MAP_WIDGET_LIB}
        config_codec
        road_graph
        spdlog::spdlog
)

set_target_properties(map_test_widget PROPERTIES AUTOMOC ON)

add_project_test(TARGET map_test_widget LABELS dashboard map gui)

# The same assertions on a 2x screen. QT_SCALE_FACTOR is what gives an offscreen
# widget a device pixel ratio -- there is no display to take one from -- and it
# is the only way to reach the paint path the map renders its geometry through
# on a HiDPI panel. Without this the ratio is 1 in every test on the machine and
# the map could go back to rendering at half resolution unnoticed.
add_project_test(TARGET map_test_widget NAME map_test_widget_hidpi
                 LABELS dashboard map gui ENVIRONMENT QT_SCALE_FACTOR=2)
