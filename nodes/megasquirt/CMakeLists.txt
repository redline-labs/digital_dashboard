cmake_minimum_required(VERSION 3.10)

project(megasquirt)

add_executable(megasquirt
    main.cpp
)

target_include_directories(megasquirt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(megasquirt PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    schemas
    zenoh_pub_sub
    dbc_megasquirt_dash_data
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
