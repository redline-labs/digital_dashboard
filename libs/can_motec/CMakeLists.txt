cmake_minimum_required(VERSION 3.10)

project(can_motec)

# The MoTeC UTC, and the network gateways that speak the same command envelope.
#
# The codec is deliberately its own translation unit with no transport in it.
# That is what lets the interesting half -- the CRC, the payload/data-block
# split, the record layout, and the stream reassembly a 64-byte FIFO forces --
# be tested against bytes captured from real hardware on a machine with no
# dongle attached, which is the only machine this has ever been built on.
add_library(can_motec STATIC
    src/motec_gw.cpp
    src/utc_transport.cpp
    src/utc_backend.cpp
)

target_include_directories(can_motec PUBLIC include)

target_link_libraries(can_motec
    PUBLIC
        can
        helpers
    PRIVATE
        usb-1.0
        spdlog::spdlog
)

# The codec, against eleven frames a genuine UTC produced. No hardware needed,
# and none was available -- see the provenance note in tests/golden/.
add_executable(can_motec_test_gw
    tests/test_motec_gw.cpp
)

target_include_directories(can_motec_test_gw PRIVATE tests)

target_link_libraries(can_motec_test_gw
    PRIVATE
        can_motec
        spdlog::spdlog
)

add_project_test(TARGET can_motec_test_gw LABELS can motec unit)
