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

# Tests are opt-out, under CTest's conventional name. The vendored projects read
# the same variable, so third_party/CMakeLists.txt pins it OFF for its subtree --
# turning ours on never drags in capnproto's or zenoh-cpp's.
#
# Older build trees carry BUILD_TESTING=OFF, forced into the cache by
# third_party/capnproto.cmake. option() will not overwrite an existing entry, so
# that stale OFF would silently disable every test here. The forced entry is
# recognisable by its empty help string, which no option() ever writes.
get_property(_build_testing_help CACHE BUILD_TESTING PROPERTY HELPSTRING)
if(DEFINED CACHE{BUILD_TESTING} AND _build_testing_help STREQUAL "")
    unset(BUILD_TESTING CACHE)
endif()
unset(_build_testing_help)
option(BUILD_TESTING "Build this project's test suite" ON)

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
# GuiPrivate is found HERE, at the root, rather than only where it is used.
# It exposes <rhi/qrhi.h> for libs/map_render, and an imported target found in a
# subdirectory is not visible to sibling scopes -- the dashboard executable
# links map_render and Qt's dependency walker warns that GuiPrivate "is
# mentioned as a dependency but not declared". Declaring it at the root is what
# makes that walk complete.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Multimedia GuiPrivate)
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
if(BUILD_TESTING)
    enable_testing()
endif()
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(ProjectTest)
include(DashboardWidget)


# Sanitizers, for our code and its tests only: this lands after
# add_subdirectory(third_party), like the warning block below, so the vendored
# libraries are not instrumented. Off by default; use a separate build tree:
#
#     cmake -S . -B build-asan -DREDLINE_SANITIZE=address,undefined
#     cmake --build build-asan && ctest --test-dir build-asan -L unit
#
# Undefined behaviour aborts rather than printing and carrying on, so a test
# that trips it fails. The release default compiles assert() and capnp's bounds
# checks out; this is the build that notices an out-of-bounds read anyway.
set(REDLINE_SANITIZE "" CACHE STRING
    "Sanitizers for our code and tests, e.g. address,undefined (empty = off)")
if(REDLINE_SANITIZE)
    add_compile_options(
        -fsanitize=${REDLINE_SANITIZE}
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -g
    )
    add_link_options(-fsanitize=${REDLINE_SANITIZE})
endif()

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
    # Both, because the compilers draw the line differently: clang's
    # -Wconversion already includes the sign changes, GCC's leaves them to
    # -Wsign-conversion. A narrowing, a sign flip or an int-to-float that can
    # lose precision has to be written out where it is meant, so the ones that
    # are not meant stop compiling.
    -Wconversion
    -Wsign-conversion
    -Wuninitialized
    -Wmissing-field-initializers
    # -Wmissing-field-initializers means two different things. Both compilers
    # apply it to positional aggregate initialization; only GCC also applies it
    # to C++20 DESIGNATED initializers, where a member left out has to have a
    # default member initializer or it warns. Clang split that half off into its
    # own flag (clang 18+) and leaves it off, so a designated initializer that
    # skips a defaultless member compiles silently here and is a -Werror stop on
    # a Linux build. Turn clang's half back on rather than find out on the other
    # machine. GCC does not know the spelling, hence the guard.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-designated-field-initializers>
    #$<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>

    -Wshadow
    # Same split as the designated-initializer flag above. GCC's -Wshadow counts
    # a lambda's local as shadowing an enclosing function's local even when the
    # lambda does not capture it; clang parks that case in its own flag and
    # leaves it off, so the shadow only surfaces on a Linux build.
    #
    # This one is STRICTER than GCC, not equal to it: clang also flags the
    # `[x = std::move(x)]` init-capture, which GCC accepts and which is
    # perfectly ordinary. There is no way to ask clang for only the first half,
    # so the tree spells those captures with distinct names instead. If that
    # ever costs more than it catches, this line is the thing to delete.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshadow-uncaptured-local>
    #-Wsuggest-final-types
    #-Wsuggest-final-methods

    -Wno-expansion-to-defined
    -Wswitch-enum

    # CLOSING THE GAP BETWEEN THE TWO COMPILERS. Developed on clang, shipped by
    # Yocto on GCC 15, and the two do not diagnose the same set -- which is free
    # coverage, not a nuisance to waive at the integration end. GCC found a real
    # use-after-free on the pub/sub RX path that clang cannot spell at all; see
    # -Wdangling-reference below and byte_subscriber.cpp.
    #
    # The rule: a warning in OUR code gets FIXED, never downgraded. Only system
    # and third-party headers get waived, and that is the integrator's business
    # -- the libstdc++ -Warray-bounds false positive the recipe downgrades does
    # reproduce here under GCC 16 at -O2, and nothing in the application changes
    # it. A flag belongs here only if the tree builds clean under it; the ones
    # that did not are recorded below with the reason.

    # Both compilers, C++ only: the two ways a class hierarchy silently does the
    # wrong thing at runtime, over a tree full of polymorphic seams.
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>

    # NOT -Wextra-semi, NOT -Wundef: both clean in our code, both fire only from
    # vendored headers (zenoh-cpp and earcut write `};`, kj/windows-sanity.h
    # tests _WIN32). Passing them means moving those to -isystem, which
    # third_party/earcut.cmake explains this tree deliberately does not do.

    # Both compilers, any language. -Wimplicit-fallthrough is in GCC's -Wextra
    # but not in clang's for C++, so a fallthrough that stops the Yocto build
    # compiles silently here -- the exact shape this block exists to retire.
    -Wimplicit-fallthrough

    # NOT -Wdouble-promotion, and this one looks like it belongs: 169 hits, no
    # defects. Qt's geometry API is all qreal and this tree computes in float,
    # so every hit is a widening at an API boundary. Silencing it means casts at
    # 169 call sites to remove a warning that was never about a bug.

    # GCC only, each a class clang cannot see. -Wdangling-reference is in GCC's
    # -Wall from 13, but naming it means a GCC 13/14 builder gets it too.
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-Wdangling-reference>
    $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond>
    $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-branches>
    $<$<CXX_COMPILER_ID:GNU>:-Wlogical-op>

    # Clang only, the other direction. -Wconditional-uninitialized is clang's
    # answer to -Wmaybe-uninitialized and stricter than the -Wuninitialized we
    # were relying on; -Wheader-hygiene catches `using namespace` at header
    # scope, which GCC has no flag for.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wconditional-uninitialized>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wloop-analysis>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wheader-hygiene>
)


add_subdirectory(schemas)
add_subdirectory(libs)
add_subdirectory(dbcs)
add_subdirectory(eds)
add_subdirectory(apps)
add_subdirectory(mock_data)
add_subdirectory(nodes)
# Workstation-only tools. Placed HERE, beside nodes, rather than earlier: the
# -Werror block above applies from add_subdirectory(schemas) onwards, and a
# tools/ added above it would silently opt out of every warning this tree treats
# as load-bearing.
add_subdirectory(tools)

# Local docs preview (manual): serves the Jekyll site from the docs directory.
# Usage: cmake --build . --target docs_serve
add_custom_target(docs_serve
    COMMAND bundle exec jekyll serve --baseurl=
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/docs
    USES_TERMINAL
    COMMENT "Serving docs via Jekyll at http://127.0.0.1:4000 (baseurl='')"
)
