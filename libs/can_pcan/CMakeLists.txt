cmake_minimum_required(VERSION 3.10)

project(can_pcan)

# PCAN-USB FD adapters over libusb.
#
# The split here is deliberate. pucan.cpp is the wire protocol -- command
# framing, record decoding, bit-timing register packing, FD length coding -- and
# has no libusb in it at all, so every part of this driver that can be wrong
# without hardware attached is testable without hardware attached. The libusb
# half is transfers and thread lifetime, and there is not much of it.
#
# Builds on macOS and Linux alike; libusb is portable and so is everything here.
add_library(can_pcan STATIC
    src/pucan.cpp
    src/pcan_device.cpp
    src/pcan_backend.cpp
)

target_include_directories(can_pcan PUBLIC include)
target_include_directories(can_pcan PRIVATE src)

target_link_libraries(can_pcan
    PUBLIC
        can
        helpers
    PRIVATE
        usb-1.0
        spdlog::spdlog
)

# The wire protocol, byte by byte. No adapter required, which is the point:
# a flag in the wrong bit or a length counted from the wrong place is exactly
# the sort of thing that is invisible until it is on a vehicle.
add_executable(can_pcan_test_pucan
    tests/test_pucan.cpp
)

target_link_libraries(can_pcan_test_pucan
    PRIVATE
        can_pcan
        spdlog::spdlog
)

add_project_test(TARGET can_pcan_test_pucan LABELS can pcan unit)
