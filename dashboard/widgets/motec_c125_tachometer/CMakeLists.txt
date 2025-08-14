cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(motec_c125_tachometer STATIC
    motec_c125_tachometer.cpp
)

target_include_directories(motec_c125_tachometer PUBLIC
    include
)

target_link_libraries(motec_c125_tachometer
    PRIVATE
        Qt6::Widgets
        common_logging
        expression_parser
        helpers
)



