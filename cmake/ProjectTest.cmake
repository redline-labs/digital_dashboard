# SPDX-License-Identifier: GPL-3.0-or-later
#
# One way to register a test, so `ctest` sees all of them and they can be
# selected as a group.
#
#     add_project_test(TARGET airplay_test_hid LABELS airplay unit)
#
# Targets are named <component>_test_<subject> throughout, so `ctest -N` reads
# as a grouped list and `ctest -R <component>` selects one library's tests.
#
# Labels are how you pick a subset. Every test carries its component
# (airplay, plist, iap2, apple_usb, pub_sub, canopen, dashboard) plus at least
# one of:
#
#   unit   pure logic. No sockets, no clock, no hardware, no display.
#          Deterministic and fast. This is the set to run on every build.
#   net    opens a zenoh session, so it talks to the loopback network and can
#          be perturbed by anything else running a session on the machine.
#   gui    constructs Qt widgets. Forced onto the offscreen platform here so it
#          does not need a display.
#   slow   takes more than a second, usually because it measures real elapsed
#          time. Excluded by `ctest -LE slow` when you want a quick answer.
#
# A test registered here must FAIL by exit code. A program that only prints and
# always returns 0 is a demo, not a test -- register it as a target if it is
# useful, but do not add_project_test() it, because a green ctest run would then
# be telling you nothing.

function(add_project_test)
    cmake_parse_arguments(PT "" "TARGET;TIMEOUT" "LABELS" ${ARGN})

    if(NOT PT_TARGET)
        message(FATAL_ERROR "add_project_test: TARGET is required")
    endif()
    if(NOT TARGET ${PT_TARGET})
        message(FATAL_ERROR "add_project_test: no such target '${PT_TARGET}'")
    endif()
    if(NOT PT_LABELS)
        message(FATAL_ERROR "add_project_test(${PT_TARGET}): at least one LABEL is required")
    endif()

    add_test(NAME ${PT_TARGET} COMMAND ${PT_TARGET})
    set_tests_properties(${PT_TARGET} PROPERTIES LABELS "${PT_LABELS}")

    # Default is generous: these are correctness tests, not benchmarks, and a
    # hang should be reported as a hang rather than by wedging the whole run.
    if(NOT PT_TIMEOUT)
        set(PT_TIMEOUT 120)
    endif()
    set_tests_properties(${PT_TARGET} PROPERTIES TIMEOUT ${PT_TIMEOUT})

    # A Qt test must not need a display. The test binaries set this themselves
    # before constructing QApplication, but setting it here too means running
    # one by hand behaves the same way as running it under ctest.
    if("gui" IN_LIST PT_LABELS)
        set_tests_properties(${PT_TARGET} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endfunction()
