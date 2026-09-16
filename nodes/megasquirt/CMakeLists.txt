cmake_minimum_required(VERSION 3.10)

project(megasquirt)

# The decoded frames onto the schema. A library so the mapping can be tested
# without a bus; see megasquirt_messages.h.
add_library(megasquirt_messages STATIC
    megasquirt_messages.cpp
)

target_include_directories(megasquirt_messages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(megasquirt_messages PUBLIC
    capnp
    schemas
    dbc_megasquirt_dash_data
)

add_executable(megasquirt
    main.cpp
)

target_include_directories(megasquirt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(megasquirt PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    schemas
    zenoh_pub_sub
    node_health
    cli
    dbc_megasquirt_dash_data
    megasquirt_messages
)

add_executable(megasquirt_test_frames
    test_frames.cpp
)

target_link_libraries(megasquirt_test_frames PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    schemas
    zenoh_pub_sub
    dbc_megasquirt_dash_data
)

# The mapping, field by field, with a distinct value in every source field so a
# transposition reads as the wrong number rather than as a coincidence.
add_executable(megasquirt_test_messages
    test_megasquirt_messages.cpp
)

target_link_libraries(megasquirt_test_messages PRIVATE
    megasquirt_messages
)

add_project_test(TARGET megasquirt_test_messages LABELS megasquirt unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET megasquirt COMPONENT nodes)
redline_install(TARGET megasquirt_test_frames COMPONENT nodes)
