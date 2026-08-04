cmake_minimum_required(VERSION 3.10)

project(segment_readout)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_library(segment_readout
    segment_readout.cpp
    include/segment_readout/segment_readout.h
)

target_include_directories(segment_readout PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(segment_readout
    PUBLIC
        reflection
        helpers # config.h exposes helpers::Color
    PRIVATE
        Qt6::Widgets
        spdlog::spdlog
        zenoh_pub_sub
)
