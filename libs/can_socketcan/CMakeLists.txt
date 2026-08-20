cmake_minimum_required(VERSION 3.10)

project(can_socketcan)

# SocketCAN, the kernel's own CAN stack.
#
# Built on every platform, not just Linux. Two of the three source files are
# pure byte manipulation -- the frame layouts and the netlink messages that set
# a bit rate -- and those are the parts most able to be silently wrong, so they
# are compiled and tested on a developer's macOS box rather than only on the
# target. The socket layer itself is behind an `#if defined(__linux__)`, and off
# Linux the backend still exists but reports every channel as unsupported. That
# keeps a node's channel list meaning the same thing everywhere.
add_library(can_socketcan STATIC
    src/netlink.cpp
    src/socketcan_frame.cpp
    src/socketcan_backend.cpp
)

target_include_directories(can_socketcan PUBLIC include)

target_link_libraries(can_socketcan
    PUBLIC
        can
        helpers
    PRIVATE
        spdlog::spdlog
)

# The frame flag bits and the netlink message layout. No CAN interface needed,
# which matters because the Linux socket code around them cannot be exercised
# here at all.
add_executable(can_socketcan_test
    tests/test_socketcan.cpp
)

# tests/golden/ holds a netlink reply captured from a real kernel; see the
# header for how it was taken.
target_include_directories(can_socketcan_test PRIVATE tests)

target_link_libraries(can_socketcan_test
    PRIVATE
        can_socketcan
        spdlog::spdlog
)

add_project_test(TARGET can_socketcan_test LABELS can socketcan unit)
