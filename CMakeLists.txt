cmake_minimum_required(VERSION 3.10)

# Let CMake de-duplicate static libraries on the link line.  By default it keeps
# a target's direct dependencies in their original order and appends the
# transitive ones, so an archive named both directly and indirectly (spdlog,
# schemas) is passed twice and Apple's linker warns "ignoring duplicate
# libraries".  We have no order-sensitive link entries (no intermixed link flags,
# no two libraries providing the same symbol), so free reordering is safe.
# Requires CMake 3.31+; older versions ignore the variable and just keep the
# duplicates.
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

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
project(dashboard LANGUAGES C CXX)

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

    # Optimization
    -ffunction-sections
    -fdata-sections
    #-mlong-calls
    -fno-math-errno
    #-flto
)

# Homebrew on Intel installs into /usr/local, and clang searches
# /usr/local/include by default without marking it a system directory the way it
# marks the SDK ones.  Two consequences, both ours to deal with: it sits ahead of
# the -isystem paths pkg-config hands us, so ffmpeg and OpenSSL resolve through
# it rather than through their Cellar prefix, and warnings from what it finds are
# attributed to us -- which turns every old-style cast in those headers into a
# -Werror failure.  Apple Silicon never sees this because /opt/homebrew is not on
# the default search path at all, so the -isystem entry is the only way there.
#
# Re-flagging the directory as -isystem corrects the classification without
# introducing a search path that was not already being searched.
# include_directories(SYSTEM) cannot do it: CMake drops any directory that is
# already an implicit compiler include, and that is precisely this one.
if(APPLE)
    foreach(implicit_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        if(implicit_dir MATCHES "^(/usr/local|/opt/homebrew)/include$")
            message(STATUS "Treating ${implicit_dir} as a system include directory")
            add_compile_options("SHELL:-isystem ${implicit_dir}")
        endif()
    endforeach()
endif()

add_subdirectory(third_party)

# Tests. Deliberately enabled *after* third_party: the vendored projects declare
# their own CTest suites, and without testing enabled in their directory scope
# those declarations are dropped. Otherwise `ctest` reports a dozen zenoh-cpp
# tests we never built and cannot run, and a red run stops meaning anything.
#
#     cmake --build build -j4 && ctest --test-dir build --output-on-failure
#     ctest --test-dir build -L unit          # fast, deterministic subset
#     ctest --test-dir build -LE slow         # everything except the slow ones
#     ctest --test-dir build -L airplay -V    # one component, verbose
#
enable_testing()
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(ProjectTest)
include(DashboardWidget)


# Additional compiler options for all our code.
add_compile_options(
    #$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    #$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>

    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsuggest-override>

    # Our code only: this block lands after add_subdirectory(third_party), so
    # the vendored deps (libusb, capnproto) keep warning without failing us.
    -Werror
    #-Wcast-align
    #-Wconversion
    #-Wsign-conversion
    -Wuninitialized
    -Wmissing-field-initializers
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
add_subdirectory(eds)
add_subdirectory(dashboard)
add_subdirectory(scope)
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
