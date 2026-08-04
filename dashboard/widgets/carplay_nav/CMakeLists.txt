cmake_minimum_required(VERSION 3.10)

project(carplay_nav)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_library(carplay_nav
    carplay_nav.cpp
    include/carplay_nav/carplay_nav.h
)

target_include_directories(carplay_nav PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(carplay_nav
    PUBLIC
        reflection
        schemas
        zenoh_pub_sub
        helpers # config.h exposes helpers::Color
    PRIVATE
        Qt6::Widgets
        spdlog::spdlog
)

# The formatting rules are the only part of this widget that is not about
# pixels, so they are the part that gets a test. No Qt, no display, no bus.
add_executable(carplay_nav_test_format test_format.cpp)
target_link_libraries(carplay_nav_test_format PRIVATE carplay_nav)
add_project_test(TARGET carplay_nav_test_format LABELS carplay_nav unit)
