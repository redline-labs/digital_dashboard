cmake_minimum_required(VERSION 3.10)

project(system_info)

# What the board is and what it is doing: os-release, uptime, load, memory,
# filesystem capacity, network interfaces and hwmon temperatures.
#
# It exists as a library rather than as helpers inside nodes/web_console for one
# reason: every reader takes its root, so the parsers can be pointed at a
# fixture tree and tested on any machine. /proc is not something a test can
# arrange, and a parser that has only ever seen the host's /proc has only ever
# been tested against a single input.
#
# No zenoh, no HTTP, no Qt -- file I/O and two syscalls. Serving it is the node's
# job. Note that readTemperatures() here walks the whole hwmon class for a
# board-wide view and is deliberately NOT a replacement for
# display_backlight::readTemperatures(), which reads one display module's sensors
# from the record the rootfs wrote.
add_library(system_info STATIC
    src/system_info.cpp
)

target_include_directories(system_info PUBLIC include)

# The parsers against a fake /proc, /sys and /etc, plus the two readers that
# cannot be fixtured (statvfs, getifaddrs) asserted only on what is true of any
# host.
add_executable(system_info_test_system_info
    tests/test_system_info.cpp
)

target_link_libraries(system_info_test_system_info PRIVATE system_info)

add_project_test(TARGET system_info_test_system_info LABELS system_info unit)
