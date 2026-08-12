cmake_minimum_required(VERSION 3.10)

project(gsof)

# The Trimble GSOF protocol, and nothing else.
#
# No sockets, no threads, no zenoh, no allocation in the parse path. Everything
# a BD992 says arrives as bytes, and every way of getting those bytes wrong --
# a field offset, an endianness, a length rule for a variable-length record --
# is a mistake that produces a plausible number rather than a failure. So this
# half is separated out and made constexpr: a golden capture plus a
# static_assert turns a wrong offset into a build error, which is the only kind
# of GNSS bug you find without a receiver on the bench and a sky above it.
#
# The transport half -- TCP, reconnection, the command/response exchange -- is
# `bd992`, which depends on this. Nothing here depends on that.
add_library(gsof STATIC
    src/framer.cpp
    src/transport.cpp
)

target_include_directories(gsof PUBLIC include)

# spdlog is deliberately absent. This library reports through Result<T> and
# counters; the decision about what is worth a log line belongs to the node,
# which knows whether one bad checksum an hour matters.
target_link_libraries(gsof)

# The outer packet: checksum, framing, and resynchronisation after corruption.
add_executable(gsof_test_framing
    tests/test_framing.cpp
)

target_link_libraries(gsof_test_framing
    PRIVATE
        gsof
        spdlog::spdlog
)

add_project_test(TARGET gsof_test_framing LABELS gsof unit)

# Multi-page reassembly, including the case that makes it necessary: a record
# split across a page boundary.
add_executable(gsof_test_pages
    tests/test_pages.cpp
)

target_link_libraries(gsof_test_pages
    PRIVATE
        gsof
        spdlog::spdlog
)

add_project_test(TARGET gsof_test_pages LABELS gsof unit)

# The record parsers, against captures from real receivers. Most of this test
# runs at COMPILE time -- see the file's header comment; the executable exists
# for the handful of checks that need <cmath> and for the record walk.
add_executable(gsof_test_records
    tests/test_records.cpp
)

target_include_directories(gsof_test_records PRIVATE tests)

target_link_libraries(gsof_test_records
    PRIVATE
        gsof
        spdlog::spdlog
)

add_project_test(TARGET gsof_test_records LABELS gsof unit)

# Configuration: building APPFILE commands and reading one back. Unlike the
# record parsers these are checked against the ICD's tables rather than against
# captures -- see the caveat at the top of the file.
add_executable(gsof_test_commands
    tests/test_commands.cpp
)

target_link_libraries(gsof_test_commands
    PRIVATE
        gsof
        spdlog::spdlog
)

add_project_test(TARGET gsof_test_commands LABELS gsof unit)
