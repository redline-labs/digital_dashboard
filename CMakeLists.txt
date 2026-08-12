# 3.16 is the real floor: qt_standard_project_setup() and Qt6Config both need it,
# and FetchContent_MakeAvailable needs 3.14.  The 3.10 this used to claim was
# never true, it just went unchecked.  No <max> in the range on purpose -- naming
# one raises the policy version with it, and CMP0169 (3.30) then turns
# third_party/mcap.cmake's direct FetchContent_Populate call into a hard error.
# That call wants replacing on its own merits, but not as a side effect of
# correcting the floor.
cmake_minimum_required(VERSION 3.16)

# Let CMake de-duplicate static libraries on the link line.  By default it keeps
# a target's direct dependencies in their original order and appends the
# transitive ones, so an archive named both directly and indirectly (spdlog,
# schemas) is passed twice and Apple's linker warns "ignoring duplicate
# libraries".  We have no order-sensitive link entries (no intermixed link flags,
# no two libraries providing the same symbol), so free reordering is safe.
# Requires CMake 3.31+; older versions ignore the variable and just keep the
# duplicates.
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

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

add_subdirectory(third_party)

# Homebrew's Qt exports its entire install prefix as a system include directory.
# Qt6::Platform, which every Qt module links transitively, carries
# "${prefix}/share/qt/mkspecs/<spec>;${prefix}/include".  The mkspecs entry is
# real; the bare prefix is several hundred unrelated formulae, and it lands on
# the include path of every Qt target we build.
#
# That is not theoretical.  Anything we vendor that is also brewed then has two
# copies in scope, and the brewed one is found first: Qt's entry is emitted ahead
# of ours and both are -isystem.  It is how a brewed spdlog -- the same 1.17.0
# release, but built against an external fmt where ours bundles it -- quietly
# replaced the copy we compile against.  Nothing complained until the link, as an
# undefined fmt::v12::detail::allocate.  Ten other vendored headers collide the
# same way today (nlohmann, lz4, zstd, xxhash) and are saved only by being
# non-SYSTEM, which the compiler searches ahead of every -isystem entry.
#
# We take nothing from that directory.  Every Qt module we use is a framework, so
# its headers come from lib/Qt*.framework/Headers, and a full build resolves no
# header at all through the bare prefix.  So drop it rather than try to out-rank
# it: reordering settles only the collision we happened to notice, while removing
# the directory retires the class.  Only package-manager prefixes are stripped --
# a Qt in a prefix of its own is not a grab-bag and not a hazard.
#
# Found here rather than only in dashboard/ so every subdirectory inherits the
# corrected target; the find_package calls further down then reuse it.  If a Qt
# module ever does need the prefix, it says so at compile time with a missing
# header -- a better failure than silently compiling against the wrong copy of a
# dependency.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Multimedia)
get_target_property(QT_PLATFORM_INCLUDES Qt6::Platform INTERFACE_INCLUDE_DIRECTORIES)
foreach(prefix IN LISTS CMAKE_SYSTEM_PREFIX_PATH CMAKE_PREFIX_PATH)
    list(REMOVE_ITEM QT_PLATFORM_INCLUDES "${prefix}/include")
endforeach()
set_target_properties(Qt6::Platform PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${QT_PLATFORM_INCLUDES}")

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
