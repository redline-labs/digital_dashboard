# CarPlay Widget Library
cmake_minimum_required(VERSION 3.10)

set(CARPLAY_WIDGET_LIB carplay_widget)

# Multimedia is used for PCM playback/mic capture of the driver's audio stream.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia Gui)

add_library(${CARPLAY_WIDGET_LIB} STATIC
    carplay_widget.cpp
    include/carplay/carplay_widget.h
)

target_include_directories(${CARPLAY_WIDGET_LIB} PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    include
    ${CMAKE_SOURCE_DIR}/include
)

# libavcodec for software H.264/H.265 decode of the zenoh video stream, plus
# libavutil for the AVFrame/AVPacket allocators the decode loop calls directly.
# The prefix is target-specific: PkgConfig::<prefix> is a global name, and
# nodes/carplay declares its own ffmpeg dependency.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CARPLAY_WIDGET_LIBAV REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
)

target_link_libraries(${CARPLAY_WIDGET_LIB}
    PUBLIC
        PkgConfig::CARPLAY_WIDGET_LIBAV
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
        Qt6::Multimedia
        reflection
        schemas
        zenoh_pub_sub
        zenohcxx::zenohc
    PRIVATE
        spdlog::spdlog
)

set_target_properties(${CARPLAY_WIDGET_LIB} PROPERTIES AUTOMOC ON)
