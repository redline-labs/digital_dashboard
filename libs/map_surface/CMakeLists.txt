# The map drawn straight into a widget, with no readback.
#
# A separate target from map_render for one reason: it is a QWidget, and
# map_render deliberately is not. Everything that decides what a pixel looks
# like stays there, shared with the offscreen host; what is here is one frame
# lifecycle -- and it is here alone because this is the path that cannot run
# under QT_QPA_PLATFORM=offscreen, so it must be small enough to read rather
# than to test. See map_surface.h.
cmake_minimum_required(VERSION 3.16)

# GuiPrivate for <rhi/qrhi.h> and for the platform-integration query that
# answers whether a QRhiWidget can come up at all; Widgets for QRhiWidget
# itself.
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets)

# The headers are LISTED as sources on purpose: AUTOMOC does not scan a header
# it has not been given, and a Q_OBJECT class that silently gets no metaobject
# fails at link time a long way from the cause.
add_library(map_surface STATIC
    src/map_surface.cpp
    src/map_overlay.cpp
    src/offscreen_host.cpp
    src/rhi_widget_host.cpp
    src/map_host.h
    src/map_overlay.h
    src/offscreen_host.h
    src/rhi_widget_host.h
    include/map_surface/map_surface.h
)

# AUTOMOC does not scan headers under include/ unless they are listed as
# sources above -- a Q_OBJECT class silently gets no metaobject otherwise, and
# the symptom is a link error a long way from the cause.
set_target_properties(map_surface PROPERTIES AUTOMOC ON)

target_include_directories(map_surface PUBLIC include)

target_link_libraries(map_surface
    PUBLIC
        map_render
        Qt6::Widgets
    PRIVATE
        Qt6::GuiPrivate
        spdlog::spdlog
)

# The two hosts side by side on a real display. EXCLUDE_FROM_ALL because it
# needs a screen and a tile archive, so it is not something a build or a CI run
# should be dragging along -- build it explicitly when you want the numbers.
#
# Note the same trap map_bench has: `make` rebuilds the libraries but NOT this,
# so a stale binary will happily show you last week's behaviour.
add_executable(map_surface_bench EXCLUDE_FROM_ALL
    map_surface_bench.cpp
)

target_link_libraries(map_surface_bench
    PRIVATE
        map_surface
        map_render
        mbtiles
        mvt
        Qt6::Widgets
        spdlog::spdlog
)
