# SPDX-License-Identifier: GPL-3.0-or-later
#
# One way to say "this binary ships", so that the set of things a board carries
# is declared HERE, next to the target, and not rediscovered downstream.
#
#     redline_install(TARGET can_bridge COMPONENT nodes)
#
# WHY THIS EXISTS. The Yocto image (dashboard_yocto, meta-redline) used to find
# what to package by walking the build tree for ELF files at -maxdepth 3 and
# installing whatever it found. That worked, but it inferred the answer from
# build-tree shape rather than being told, and it was wrong in both directions:
# it swept up `core_test` and `apple_mfi_ic_test` (tests, which have no business
# on a vehicle) and `capnpc_schema_registry` (a code generator that runs on the
# BUILD host), while a genuinely new binary was only ever noticed because a
# separate assertion caught it linking Qt. A target added here is packaged; one
# that is not, is not. That is the whole contract.
#
# COMPONENTS map one-to-one onto the image's packages, because the split is a
# property of the software, not of the deployment:
#
#   gui       links Qt and needs a display. The cluster image only -- the
#             headless variant must never pull the Qt stack in behind one of
#             these.
#   nodes     the compute daemons and command-line tools. No graphics; they run
#             on every variant.
#   mockdata  publishers that invent vehicle data for bring-up. Separable so a
#             shipping image can leave them out without a recipe change.
#
# NOT every executable belongs here. A test registered with add_project_test()
# never does. Neither does a benchmark (map_bench, map_surface_bench are
# EXCLUDE_FROM_ALL and need a display and a tile archive), nor a build-host code
# generator: capnpc_schema_registry is run by the cross build through
# cmake/NativeCodegen.cmake, which is a different question from what a board
# runs.
#
# DESTINATION is `bin` under whatever prefix the installer chooses, so the
# deployment picks the location:
#
#     DESTDIR=<staging> cmake --install <build> --prefix /opt/redline \
#         --component nodes

# Included from more than one place: the root CMakeLists (for the tree) and
# third_party/zenohd.cmake, which runs during add_subdirectory(third_party) --
# long before the root gets to include(ProjectInstall) at line ~127. Guarded and
# self-includable for the same reason NativeCodegen.cmake is: a call site should
# get the contract without having to depend on include order.
include_guard(GLOBAL)

set(REDLINE_INSTALL_COMPONENTS gui nodes mockdata
    CACHE INTERNAL "Valid redline_install() components")

function(redline_install)
    cmake_parse_arguments(RI "" "TARGET;COMPONENT" "" ${ARGN})

    if(NOT RI_TARGET)
        message(FATAL_ERROR "redline_install: TARGET is required")
    endif()
    if(NOT TARGET ${RI_TARGET})
        message(FATAL_ERROR "redline_install: no such target '${RI_TARGET}'")
    endif()
    if(NOT RI_COMPONENT)
        message(FATAL_ERROR "redline_install(${RI_TARGET}): COMPONENT is required, "
                            "one of: ${REDLINE_INSTALL_COMPONENTS}")
    endif()
    if(NOT RI_COMPONENT IN_LIST REDLINE_INSTALL_COMPONENTS)
        message(FATAL_ERROR "redline_install(${RI_TARGET}): COMPONENT '${RI_COMPONENT}' "
                            "is not one of: ${REDLINE_INSTALL_COMPONENTS}")
    endif()

    # A target excluded from `all` is not built by an ordinary build, so
    # installing it would fail at install time with a missing file -- long after
    # the mistake was made. Say so here instead.
    get_target_property(_excluded ${RI_TARGET} EXCLUDE_FROM_ALL)
    if(_excluded)
        message(FATAL_ERROR
            "redline_install(${RI_TARGET}): the target is EXCLUDE_FROM_ALL, so a "
            "normal build never produces it and installing it cannot work. Either "
            "build it by default or do not ship it.")
    endif()

    # BUNDLE DESTINATION is here for the macOS developer build, not for any
    # image we ship: `editor` sets MACOSX_BUNDLE so its menu bar reads
    # "Dashboard Editor", and install(TARGETS) refuses a bundle target outright
    # if it is not told where one goes. The boards are Linux, where MACOSX_BUNDLE
    # is inert and RUNTIME DESTINATION is the one that applies.
    install(TARGETS ${RI_TARGET}
            RUNTIME DESTINATION bin
            BUNDLE DESTINATION bin
            COMPONENT ${RI_COMPONENT})
endfunction()

# redline_install_program(PROGRAM <path> COMPONENT <component>)
#
# The same contract for something the build PRODUCES BUT DOES NOT OWN AS A
# TARGET. install(TARGETS) cannot take the output of an add_custom_command, and
# zenohd is exactly that: it is built by cargo out of the zenoh workspace
# third_party/zenoh-c.cmake already fetches, patches and SHA-verifies, so there
# is a file to ship but no CMake target to name.
#
# Kept as a separate function rather than overloading redline_install(), because
# the two cannot validate the same way: a target can be checked for
# EXCLUDE_FROM_ALL before it is built, a path cannot be checked for anything
# until install time. Reaching for this when redline_install() would do is a
# mistake -- it gives up that check.
#
# DEPENDS is the caller's problem: the custom target that produces the file has
# to be in `all`, or the install lands on a path that was never written.
function(redline_install_program)
    cmake_parse_arguments(RIP "" "PROGRAM;COMPONENT" "" ${ARGN})

    if(NOT RIP_PROGRAM)
        message(FATAL_ERROR "redline_install_program: PROGRAM is required")
    endif()
    if(NOT RIP_COMPONENT)
        message(FATAL_ERROR "redline_install_program(${RIP_PROGRAM}): COMPONENT is "
                            "required, one of: ${REDLINE_INSTALL_COMPONENTS}")
    endif()
    if(NOT RIP_COMPONENT IN_LIST REDLINE_INSTALL_COMPONENTS)
        message(FATAL_ERROR "redline_install_program(${RIP_PROGRAM}): COMPONENT "
                            "'${RIP_COMPONENT}' is not one of: ${REDLINE_INSTALL_COMPONENTS}")
    endif()

    install(PROGRAMS ${RIP_PROGRAM}
            DESTINATION bin
            COMPONENT ${RIP_COMPONENT})
endfunction()
