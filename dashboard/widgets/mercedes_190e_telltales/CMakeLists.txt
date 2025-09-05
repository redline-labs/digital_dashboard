cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)

# Create the library with all the carplay source files
add_library(mercedes_190e_telltales STATIC
    telltale.cpp
)

# Set up include directories for this library
target_include_directories(mercedes_190e_telltales PUBLIC
    include
    #${exprtk_SOURCE_DIR}  # ExprTk is header-only
    ${CMAKE_CURRENT_BINARY_DIR}/../../_deps/exprtk-src
    ${CMAKE_CURRENT_BINARY_DIR}/../../_deps/capnproto-src/c++/src  # Cap'n Proto headers
)

# Link required libraries to this widget
target_link_libraries(mercedes_190e_telltales
    PUBLIC
        reflection
    PRIVATE
        Qt6::Widgets
        Qt6::Svg
        zenohcxx::zenohc  # For Zenoh functionality
        common_logging    # For spdlog
        schemas           # For Cap'n Proto schemas
        zenoh_pub_sub     # For condition evaluation
        helpers
)
