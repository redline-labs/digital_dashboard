cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(circle_tachometer STATIC
    circle_tachometer.cpp
)

target_include_directories(circle_tachometer PUBLIC
    include
)

target_link_libraries(circle_tachometer
    PRIVATE
        Qt6::Widgets
        zenohcxx::zenohc
        common_logging
        schemas
        expression_parser
)



