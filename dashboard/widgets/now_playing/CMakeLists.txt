cmake_minimum_required(VERSION 3.10)

project(now_playing)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_library(now_playing
    now_playing.cpp
    include/now_playing/now_playing.h
)

target_include_directories(now_playing PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(now_playing
    PUBLIC
        reflection
        schemas
        zenoh_pub_sub
    PRIVATE
        Qt6::Widgets
        spdlog::spdlog
)
