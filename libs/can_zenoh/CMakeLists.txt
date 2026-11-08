# CAN <-> zenoh, both ways.
#
# BridgedChannel puts a can::Channel on a pair of topics -- received frames out
# on rx, frames to transmit in on tx, a .trc tap, a status entry. can_bridge
# does that for adapters; a simulator does it for the device it simulates.
#
# The zenoh: backend is the other end of those topics as a can::Channel, so a
# tool written against can::Channel reaches a remote bus, or a simulated one,
# exactly as it reaches a local adapter.
#
# After can_trc (the recording tap) and pub_sub; before can_backends, which
# offers the zenoh: backend behind a flag.
add_library(can_zenoh STATIC
    src/bridged_channel.cpp
    src/trc_recorder.cpp
    src/zenoh_backend.cpp
)

target_include_directories(can_zenoh PUBLIC include)

target_link_libraries(can_zenoh
    PUBLIC
        can
        can_trc
        helpers
        zenoh_pub_sub
        schemas
        capnp
    PRIVATE
        spdlog::spdlog
)

# ---------------------------------------------------------------------- tests

# The transmit queue between the tx topic and the adapter.
add_executable(can_zenoh_test_tx_queue tests/test_tx_queue.cpp)
target_link_libraries(can_zenoh_test_tx_queue PRIVATE can_zenoh)
add_project_test(TARGET can_zenoh_test_tx_queue LABELS can_zenoh unit)

# The recording tap. What is pinned here is that both directions land in the
# file with the right direction and in the recorded order, that a full queue
# drops and counts rather than blocking the thread that feeds it, and that
# hardware timestamps -- whose epoch differs per backend -- still produce
# sensible offsets.
add_executable(can_zenoh_test_recorder tests/test_trc_recorder.cpp)
target_link_libraries(can_zenoh_test_recorder PRIVATE can_zenoh spdlog::spdlog)
add_project_test(TARGET can_zenoh_test_recorder LABELS can_zenoh unit)

# A channel whose hardware goes away is reported lost, and one opened in its
# place carries traffic on the same topic. Over the virtual bus and a real
# zenoh session.
add_executable(can_zenoh_test_reconnect tests/test_reconnect.cpp)
target_link_libraries(can_zenoh_test_reconnect PRIVATE can_zenoh spdlog::spdlog)
add_project_test(TARGET can_zenoh_test_reconnect LABELS can_zenoh net)

# The tx topic's callback queues rather than waiting on a blocked send().
add_executable(can_zenoh_test_transmit tests/test_transmit.cpp)
target_link_libraries(can_zenoh_test_transmit PRIVATE can_zenoh spdlog::spdlog)
add_project_test(TARGET can_zenoh_test_transmit LABELS can_zenoh net)

# zenoh: against a bridge: both directions with every flag, then a
# wrong-schema stream and frames whose length disagrees with their payload.
add_executable(can_zenoh_test_backend tests/test_backend.cpp)
target_link_libraries(can_zenoh_test_backend PRIVATE can_zenoh spdlog::spdlog)
add_project_test(TARGET can_zenoh_test_backend LABELS can_zenoh net)
