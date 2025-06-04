cmake_minimum_required(VERSION 3.10)

# Include FetchContent module for downloading dependencies
include(FetchContent)

set(CMAKE_C_FLAGS_DEBUG "-O1 -g3" CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g3" CACHE STRING "")
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O2" CACHE STRING "")

# project name
project(carplay_cpp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# we default to Release build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()
message("-- Build Type: ${CMAKE_BUILD_TYPE}")

# Fetch spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
    GIT_SHALLOW TRUE
    # Option 1: Apply patch during fetch (before source is processed)
    PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/patches/spdlog_tweakme.patch
)

# Configure spdlog options before making it available
# Disable building tests, examples, and benchmarks
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spdlog)

# Fetch nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# Configure nlohmann/json options
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json)

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

add_subdirectory(libusb-cmake-1.0.27)

# Add widgets subdirectory
add_subdirectory(widgets)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets)
qt_standard_project_setup()


include_directories(
    SYSTEM
    yaml-cpp-0.8.0/include

    libusb-cmake-1.0.27/libusb

    cxxopts-3.2.0/include
)

include_directories(
    include
)

qt_add_executable(${PROJECT_NAME}
    main.cpp
    app_config.cpp
    main_window.cpp

    yaml-cpp-0.8.0/src/binary.cpp
    yaml-cpp-0.8.0/src/convert.cpp
    yaml-cpp-0.8.0/src/depthguard.cpp
    yaml-cpp-0.8.0/src/directives.cpp
    yaml-cpp-0.8.0/src/emit.cpp
    yaml-cpp-0.8.0/src/emitfromevents.cpp
    yaml-cpp-0.8.0/src/emitter.cpp
    yaml-cpp-0.8.0/src/emitterstate.cpp
    yaml-cpp-0.8.0/src/emitterutils.cpp
    yaml-cpp-0.8.0/src/exceptions.cpp
    yaml-cpp-0.8.0/src/exp.cpp
    yaml-cpp-0.8.0/src/memory.cpp
    yaml-cpp-0.8.0/src/node.cpp
    yaml-cpp-0.8.0/src/node_data.cpp
    yaml-cpp-0.8.0/src/nodebuilder.cpp
    yaml-cpp-0.8.0/src/nodeevents.cpp
    yaml-cpp-0.8.0/src/null.cpp
    yaml-cpp-0.8.0/src/ostream_wrapper.cpp
    yaml-cpp-0.8.0/src/parse.cpp
    yaml-cpp-0.8.0/src/parser.cpp
    yaml-cpp-0.8.0/src/regex_yaml.cpp
    yaml-cpp-0.8.0/src/scanner.cpp
    yaml-cpp-0.8.0/src/scanscalar.cpp
    yaml-cpp-0.8.0/src/scantag.cpp
    yaml-cpp-0.8.0/src/scantoken.cpp
    yaml-cpp-0.8.0/src/simplekey.cpp
    yaml-cpp-0.8.0/src/singledocparser.cpp
    yaml-cpp-0.8.0/src/stream.cpp
    yaml-cpp-0.8.0/src/tag.cpp
)

#set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets

    # Link spdlog (now fetched via FetchContent)
    spdlog::spdlog

    # Link nlohmann/json (now fetched via FetchContent)
    nlohmann_json::nlohmann_json

    # Link the carplay widget library
    carplay_widget
)

