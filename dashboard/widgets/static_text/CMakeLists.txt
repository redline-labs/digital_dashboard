cmake_minimum_required(VERSION 3.10)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(static_text STATIC
    static_text.cpp
)

target_include_directories(static_text PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    include
)

target_link_libraries(static_text
    PUBLIC
        reflection
        helpers
    PRIVATE
        Qt6::Widgets
        common_logging
)


