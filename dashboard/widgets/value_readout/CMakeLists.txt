cmake_minimum_required(VERSION 3.10)

project(value_readout)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_library(value_readout
    value_readout.cpp
)

target_include_directories(value_readout PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/dashboard/widgets/value_readout/include
)

target_link_libraries(value_readout PRIVATE
    Qt6::Widgets
    spdlog::spdlog
    common_logging
    expression_parser
)


