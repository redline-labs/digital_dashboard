cmake_minimum_required(VERSION 3.10)

project(backlight_node)

# A display module's backlight and sensors on the bus: status out, brightness in.
#
# Everything that touches a file is libs/display_backlight. What is here is the
# config, the light-sensor thread, and the translation onto schemas.
add_executable(backlight
    main.cpp
    node_config.cpp
)

target_link_libraries(backlight PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    display_backlight
    yaml-cpp::yaml-cpp
    node_health
    cli
)

add_executable(backlight_test_node_config
    test_node_config.cpp
    node_config.cpp
)

target_include_directories(backlight_test_node_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(backlight_test_node_config PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
    yaml-cpp::yaml-cpp
)

add_project_test(TARGET backlight_test_node_config LABELS backlight unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET backlight COMPONENT nodes)
