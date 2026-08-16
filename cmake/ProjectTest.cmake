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
#   net    opens a zenoh session, so it talks to the loopback network and pays
#          the runtime's startup and teardown. It can no longer be perturbed by
#          another session on the machine: every test runs with peer discovery
#          off, so it neither finds nor is found by anything. See the
#          PUB_SUB_NO_DISCOVERY note below.
#   gui    constructs Qt widgets. Forced onto the offscreen platform here so it
#          does not need a display.
#   slow   takes more than a second, usually because it measures real elapsed
#          time. Excluded by `ctest -LE slow` when you want a quick answer.
#
# A test registered here must FAIL by exit code. A program that only prints and
# always returns 0 is a demo, not a test -- register it as a target if it is
# useful, but do not add_project_test() it, because a green ctest run would then
# be telling you nothing.
#
# One binary may be registered more than once, when the same assertions are
# worth running under a different environment:
#
#     add_project_test(TARGET map_test_widget NAME map_test_widget_hidpi
#                      LABELS dashboard map gui ENVIRONMENT QT_SCALE_FACTOR=2)
#
# NAME defaults to TARGET, and must be given when registering a target twice --
# ctest test names are unique. ENVIRONMENT entries are APPENDED to the ones this
# function sets, never replacing them.

function(add_project_test)
    cmake_parse_arguments(PT "" "TARGET;NAME;TIMEOUT" "LABELS;ENVIRONMENT" ${ARGN})

    if(NOT PT_TARGET)
        message(FATAL_ERROR "add_project_test: TARGET is required")
    endif()
    if(NOT TARGET ${PT_TARGET})
        message(FATAL_ERROR "add_project_test: no such target '${PT_TARGET}'")
    endif()
    if(NOT PT_LABELS)
        message(FATAL_ERROR "add_project_test(${PT_TARGET}): at least one LABEL is required")
    endif()

    if(NOT PT_NAME)
        set(PT_NAME ${PT_TARGET})
    endif()

    add_test(NAME ${PT_NAME} COMMAND ${PT_TARGET})
    set_tests_properties(${PT_NAME} PROPERTIES LABELS "${PT_LABELS}")

    # Default is generous: these are correctness tests, not benchmarks, and a
    # hang should be reported as a hang rather than by wedging the whole run.
    if(NOT PT_TIMEOUT)
        set(PT_TIMEOUT 120)
    endif()
    set_tests_properties(${PT_NAME} PROPERTIES TIMEOUT ${PT_TIMEOUT})

    # ACCUMULATED, because set_tests_properties(ENVIRONMENT) REPLACES rather than
    # appends -- setting it twice silently drops the first one.
    # Seeded with the caller's, so a registration that asks for an extra
    # variable keeps the ones below rather than choosing between them.
    set(PT_ENVIRONMENT ${PT_ENVIRONMENT})

    # A Qt test must not need a display. The test binaries set this themselves
    # before constructing QApplication, but setting it here too means running
    # one by hand behaves the same way as running it under ctest.
    if("gui" IN_LIST PT_LABELS)
        list(APPEND PT_ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    # NO TEST JOINS THE MACHINE'S BUS, whether or not it is labelled `net`.
    #
    # Applied to every test rather than to the ones that look like they open a
    # zenoh session, because the test this was found on -- scope_test_panels --
    # is labelled `gui scope` and opens one anyway, through a window that builds
    # a live source three layers down. A rule that depends on remembering to
    # label a test correctly is a rule that fails exactly when someone forgets.
    #
    # Two sessions that find each other and then close together DEADLOCK in
    # zenoh's teardown, which is what `ctest -j8` was doing: an intermittent
    # 120 s timeout on whichever test happened to overlap another. They also
    # share a bus, so one test's samples land in another's subscriber. See the
    # note in pub_sub/session_manager.cpp.
    list(APPEND PT_ENVIRONMENT "PUB_SUB_NO_DISCOVERY=1")

    set_tests_properties(${PT_NAME} PROPERTIES ENVIRONMENT "${PT_ENVIRONMENT}")
endfunction()
