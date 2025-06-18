cmake_minimum_required(VERSION 3.10)

# Include FetchContent module for downloading dependencies
include(FetchContent)

set(CMAKE_C_FLAGS_DEBUG "-O1 -g3" CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g3" CACHE STRING "")
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O2" CACHE STRING "")

# Workaround for macOS
SET(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)

# project name
project(carplay_cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# we default to Release build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()
message("-- Build Type: ${CMAKE_BUILD_TYPE}")



# Only fetch dependencies during initial configuration, not on every build
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(FETCHCONTENT_QUIET OFF)

# Include dependency configurations
include(cmake/spdlog.cmake)
include(cmake/nlohmann_json.cmake)
include(cmake/cxxopts.cmake)
include(cmake/yaml-cpp.cmake)
include(cmake/libusb.cmake)
include(cmake/zenoh-c.cmake)
include(cmake/zenoh-cpp.cmake)

# LGPL Compliance: Create source code availability notice
file(WRITE ${CMAKE_BINARY_DIR}/licenses/SOURCE_CODE_NOTICE.txt
"LGPL Source Code Availability Notice
=====================================

This software includes components licensed under the GNU Lesser General Public License (LGPL).
Per LGPL requirements, the source code for these components is available as follows:

Qt6 Framework:
- Official source: https://download.qt.io/official_releases/qt/
- License: LGPL v3
- Used version: Qt ${Qt6_VERSION} (determined at build time)

libusb:
- Official source: https://github.com/libusb/libusb
- License: LGPL v2.1+
- Used version: v1.0.27
- Git commit: See licenses/libusb/fetch_info.txt

For a copy of the LGPL licenses, see the respective license files in this directory.
")

#add_link_options(
#    -Wl,--gc-sections
#)

add_compile_options(
    #$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    #$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>

    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsuggest-override>

    #-Werror
    -Wall
    -Wextra
    #-Wcast-align
    #-Wconversion
    #-Wsign-conversion
    #$<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>

    -Wshadow
    #-Wlogical-op

    #-Wsuggest-final-types
    #-Wsuggest-final-methods

    -Wno-expansion-to-defined

    # Debugging.
    -DNDEBUG

    # Optimization
    -ffunction-sections
    -fdata-sections
    #-mlong-calls
    -fno-math-errno
    #-flto
)

add_subdirectory(widgets)


# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets Svg)
qt_standard_project_setup()


include_directories(
    include
)

qt_add_executable(${PROJECT_NAME}
    main.cpp
    app_config.cpp
    main_window.cpp
    #widgets_mainwindow.cpp
    resources.qrc
)

#set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Svg
    
    # Link spdlog (now fetched via FetchContent)
    spdlog::spdlog
    
    # Link nlohmann/json (now fetched via FetchContent)
    nlohmann_json::nlohmann_json
    
    # Link cxxopts (now fetched via FetchContent)
    cxxopts::cxxopts
    
    # Link yaml-cpp (now fetched via FetchContent)
    yaml-cpp::yaml-cpp

    zenohcxx::zenohc

    # Link the carplay widget library
    carplay_widget
    mercedes_190e_telltales
    mercedes_190e_speedometer
    mercedes_190e_tachometer
    sparkline
)

# Add comprehensive test data publisher executable
add_executable(test_data_publisher
    test_data_publisher.cpp
)

target_link_libraries(test_data_publisher PRIVATE
    # Link spdlog
    spdlog::spdlog
    
    # Link zenoh-cpp
    zenohcxx::zenohc
)

