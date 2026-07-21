cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Create the widget library
add_library(sparkline STATIC
    sparkline.cpp
)

# Set up include directories for this library
target_include_directories(sparkline PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    include
)

# Link required libraries to this widget
target_link_libraries(sparkline
    PUBLIC
        reflection
        helpers
    PRIVATE
        Qt6::Widgets
        spdlog::spdlog
        zenoh_pub_sub   # For expression evaluation if configured
)