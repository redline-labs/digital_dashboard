cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(motec_cdl3_tachometer STATIC
    motec_cdl3_tachometer.cpp
)

target_include_directories(motec_cdl3_tachometer PUBLIC
    include
)

target_link_libraries(motec_cdl3_tachometer
    PRIVATE
        Qt6::Widgets
        zenohcxx::zenohc
        common_logging
        schemas
        expression_parser
        helpers
)


