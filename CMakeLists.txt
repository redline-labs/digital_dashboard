cmake_minimum_required(VERSION 3.10)

set(CMAKE_C_FLAGS_DEBUG "-O1 -g3" CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g3" CACHE STRING "")
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O2" CACHE STRING "")

# Automatically detect and set macOS deployment target to match current system
if(APPLE)
    execute_process(
        COMMAND sw_vers -productVersion
        OUTPUT_VARIABLE MACOS_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    # Extract major.minor version (e.g., "15.5" from "15.5.0")
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" MACOS_DEPLOYMENT_VERSION "${MACOS_VERSION}")
    
    set(CMAKE_OSX_DEPLOYMENT_TARGET ${MACOS_DEPLOYMENT_VERSION})
    message(STATUS "Detected macOS version: ${MACOS_VERSION}")
    message(STATUS "Setting deployment target to: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# project name
project(dashboard LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# we default to Release build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()
message("-- Build Type: ${CMAKE_BUILD_TYPE}")

#add_link_options(
#    -Wl,--gc-sections
#)

# Compiler options for all the code including third party dependencies.
add_compile_options(
    #-Werror
    -Wall
    -Wextra

    # Debugging.
    -DNDEBUG

    # Optimization
    -ffunction-sections
    -fdata-sections
    #-mlong-calls
    -fno-math-errno
    #-flto
)

add_subdirectory(third_party)


# Additional compiler options for all our code.
add_compile_options(
    #$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    #$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>

    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsuggest-override>

    #-Werror
    #-Wcast-align
    #-Wconversion
    #-Wsign-conversion
    #$<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>

    -Wshadow
    #-Wlogical-op

    #-Wsuggest-final-types
    #-Wsuggest-final-methods

    -Wno-expansion-to-defined
    -Wswitch-enum
)


add_subdirectory(schemas)
add_subdirectory(libs)
add_subdirectory(dbcs)
add_subdirectory(dashboard)
add_subdirectory(mock_data)
add_subdirectory(nodes)

# Local docs preview (manual): serves the Jekyll site from the docs directory.
# Usage: cmake --build . --target docs_serve
add_custom_target(docs_serve
    COMMAND bundle exec jekyll serve --baseurl=
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/docs
    USES_TERMINAL
    COMMENT "Serving docs via Jekyll at http://127.0.0.1:4000 (baseurl='')"
)
