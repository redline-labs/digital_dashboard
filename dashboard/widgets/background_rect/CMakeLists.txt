cmake_minimum_required(VERSION 3.10)

project(background_rect)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_library(background_rect
    background_rect.cpp
)

target_include_directories(background_rect PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(background_rect PRIVATE
    Qt6::Widgets
    reflection
    common_logging
)


