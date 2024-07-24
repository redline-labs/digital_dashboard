cmake_minimum_required(VERSION 3.10)

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

#add_link_options(
#    -Wl,--gc-sections
#)

add_subdirectory(libusb-cmake-1.0.27)

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


include_directories(
    SYSTEM
    spdlog-1.14.1/include
    yaml-cpp-0.8.0/include

    libusb-cmake-1.0.27/libusb

    cxxopts-3.2.0/include
)

add_executable(${PROJECT_NAME}
    main.cpp
    config.cpp
    dongle_driver.cpp

    messages/message.cpp

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

target_link_libraries(${PROJECT_NAME}
    usb-1.0
)

