cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)

# Create the library with all the carplay source files
add_library(mercedes_190e_telltales STATIC
    battery_telltale.cpp
)

# Set up include directories for this library
target_include_directories(mercedes_190e_telltales PUBLIC
    include
)

# Link required libraries to this widget
target_link_libraries(mercedes_190e_telltales
    PRIVATE
        Qt6::Widgets
        Qt6::Svg
)
