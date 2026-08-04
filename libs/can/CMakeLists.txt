cmake_minimum_required(VERSION 3.10)

project(can)

# The backend-independent half: the Channel interface everything is written
# against, channel naming, bit-timing arithmetic, the FD length table, and a
# loopback backend.
#
# Deliberately free of libusb, sockets and zenoh. Everything here builds and is
# tested on any platform, which is what makes the parts most likely to be
# subtly wrong -- bit timing and FD length coding -- checkable without a CAN
# adapter on the desk.
#
# This target knows nothing about the real backends, and they depend on it.
# Assembling a registry out of whichever ones a build has is `can_backends`,
# which sits above all three; putting it here instead would make `can` depend on
# libusb to hand out a loopback channel.
add_library(can STATIC
    src/backend.cpp
    src/bitrate.cpp
    src/channel.cpp
    src/channel_id.cpp
    src/dlc.cpp
    src/error.cpp
    src/virtual_backend.cpp
)

target_include_directories(can PUBLIC include)

target_link_libraries(can
    PUBLIC
        helpers
    PRIVATE
        spdlog::spdlog
)

# Bit timing, FD length coding, channel naming, and the loopback bus. All pure
# logic, all reachable without hardware.
add_executable(can_test_core
    tests/test_can_core.cpp
)

target_link_libraries(can_test_core
    PRIVATE
        can
        spdlog::spdlog
)

add_project_test(TARGET can_test_core LABELS can unit)
