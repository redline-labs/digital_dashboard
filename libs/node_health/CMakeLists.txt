cmake_minimum_required(VERSION 3.10)

project(node_health)

# How a node says it is working, and how a monitor reads every node at once.
#
# Its own library rather than part of pub_sub: pub_sub is transport, and this
# adds a thread, a state policy and systemd. The pure parts (state, codec,
# classify) carry the logic worth testing and need no bus.
add_library(node_health STATIC
    src/state.cpp
    src/codec.cpp
    src/classify.cpp
    src/reporter.cpp
    src/monitor.cpp
)

target_include_directories(node_health PUBLIC include)

target_link_libraries(node_health
    PUBLIC
        # codec.h names the generated NodeHealth types.
        schemas
        capnp
    PRIVATE
        zenoh_pub_sub
        core
        spdlog::spdlog
)

add_executable(node_health_test_state tests/test_state.cpp)
target_link_libraries(node_health_test_state PRIVATE node_health)
add_project_test(TARGET node_health_test_state LABELS node_health unit)

# The wire mapping, including what a sample from a newer or hostile node looks
# like once it has been read.
add_executable(node_health_test_codec tests/test_codec.cpp)
target_link_libraries(node_health_test_codec PRIVATE node_health capnp kj)
add_project_test(TARGET node_health_test_codec LABELS node_health unit)

# Every verdict rule and its boundary, and the activity and continuity
# arithmetic, as pure functions of time.
add_executable(node_health_test_classify tests/test_classify.cpp)
target_link_libraries(node_health_test_classify PRIVATE node_health)
add_project_test(TARGET node_health_test_classify LABELS node_health unit)

# A reporter and a monitor on a real session: a change arrives before the next
# heartbeat, and a clean exit reads as exited rather than gone.
add_executable(node_health_test_end_to_end tests/test_end_to_end.cpp)
target_link_libraries(node_health_test_end_to_end PRIVATE node_health zenoh_pub_sub spdlog::spdlog)
add_project_test(TARGET node_health_test_end_to_end LABELS node_health net)
