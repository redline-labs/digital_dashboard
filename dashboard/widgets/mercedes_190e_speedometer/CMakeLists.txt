cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Create the library with all the carplay source files
add_library(mercedes_190e_speedometer STATIC
    mercedes_190e_speedometer.cpp
)

# Set up include directories for this library
target_include_directories(mercedes_190e_speedometer PUBLIC
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# Link required libraries to this widget
target_link_libraries(mercedes_190e_speedometer
    PUBLIC
        reflection
    PRIVATE
        Qt6::Widgets
        common_logging    # For spdlog
        zenoh_pub_sub     # For expression evaluation
        helpers
)