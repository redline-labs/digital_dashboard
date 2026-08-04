cmake_minimum_required(VERSION 3.10)

project(center_bar)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_library(center_bar
    center_bar.cpp
    include/center_bar/center_bar.h
)

target_include_directories(center_bar PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(center_bar
    PUBLIC
        reflection
        helpers # config.h exposes helpers::Color
    PRIVATE
        Qt6::Widgets
        spdlog::spdlog
        zenoh_pub_sub
)
