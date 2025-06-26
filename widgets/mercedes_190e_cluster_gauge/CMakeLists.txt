cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Create the library with all the cluster gauge source files
add_library(mercedes_190e_cluster_gauge STATIC
    mercedes_190e_cluster_gauge.cpp
)

# Set up include directories for this library
target_include_directories(mercedes_190e_cluster_gauge PUBLIC
    include
)

# Link required libraries to this widget
target_link_libraries(mercedes_190e_cluster_gauge
    PRIVATE
        Qt6::Widgets
) 