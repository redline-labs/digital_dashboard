cmake_minimum_required(VERSION 3.10)

project(msel)

# The MSEL Master Relay protocol: decoding what the isolator reports, and
# building the commands that reconfigure it.
#
# Deliberately free of zenoh and of any CAN backend. It takes frames and returns
# frames, which is what lets the whole protocol -- including the parts that only
# happen while a human holds a switch on the device -- be tested against a stub
# with nothing plugged in. Wiring it to the bus is nodes/msel_master_relay.
#
# The generated DBC code is linked PRIVATE: the decoder converts into this
# library's own types, so nothing downstream has to include a generated header
# or care that the messages came from a DBC at all.
add_library(msel STATIC
    src/decoder.cpp
    src/error.cpp
    src/protocol.cpp
    src/response_waiter.cpp
    src/stub_relay.cpp
)

target_include_directories(msel PUBLIC include)

target_link_libraries(msel
    PUBLIC
        helpers
    PRIVATE
        dbc_msel_master_relay
        spdlog::spdlog
)

# Command construction and the enum/response decoding around it, checked against
# the worked byte sequences printed in the manual.
add_executable(msel_test_protocol
    tests/test_msel_protocol.cpp
)

target_link_libraries(msel_test_protocol
    PRIVATE
        msel
        spdlog::spdlog
)

add_project_test(TARGET msel_test_protocol LABELS msel unit)

# Frame decoding: base-address rebasing, the response/status collision, short
# frames, and a round trip against the stub relay. Links the generated DBC code
# directly so it can cross-check the DBC's own view of the configuration word
# against the hand-written one.
add_executable(msel_test_decode
    tests/test_msel_decode.cpp
)

target_link_libraries(msel_test_decode
    PRIVATE
        msel
        dbc_msel_master_relay
        spdlog::spdlog
)

add_project_test(TARGET msel_test_decode LABELS msel unit)

# The hand-off between the thread that sends a configuration command and the
# thread that decodes the relay's answer. Real threads, because the cases worth
# pinning are orderings -- an answer that beats its waiter, and one that arrives
# after its caller gave up and must not be handed to the next command.
add_executable(msel_test_response_waiter
    tests/test_msel_response_waiter.cpp
)

target_link_libraries(msel_test_response_waiter
    PRIVATE
        msel
        spdlog::spdlog
)

add_project_test(TARGET msel_test_response_waiter LABELS msel unit)
