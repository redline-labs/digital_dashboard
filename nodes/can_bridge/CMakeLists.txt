cmake_minimum_required(VERSION 3.10)

project(can_bridge)

add_executable(can_bridge
    main.cpp
    bridged_channel.cpp
    node_config.cpp
    trc_recorder.cpp
)

target_link_libraries(can_bridge
    PRIVATE
    
    cli
    node_config
    core
        can
        can_backends
        can_trc
        zenoh_pub_sub
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
        capnp
        schemas
        helpers
    node_health
)

# The config surface: what an absent key defaults to, and the collisions that
# would make two channels fight over one adapter or one topic.
add_executable(can_bridge_test_config
    test_node_config.cpp
    node_config.cpp
)

target_link_libraries(can_bridge_test_config
    PRIVATE
    node_config
        can
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET can_bridge_test_config LABELS can_bridge unit)

# A channel whose hardware goes away is reported lost, and one opened in its
# place carries traffic on the same topic. Over the virtual bus and a real
# zenoh session.
add_executable(can_bridge_test_reconnect
    test_reconnect.cpp
    bridged_channel.cpp
    trc_recorder.cpp
)
target_link_libraries(can_bridge_test_reconnect PRIVATE
    can
    can_trc
    zenoh_pub_sub
    schemas
    capnp
    helpers
    spdlog::spdlog
)
add_project_test(TARGET can_bridge_test_reconnect LABELS can_bridge net)

# The health check each channel's status turns into: every bus state named.
add_executable(can_bridge_test_channel_health test_channel_health.cpp)
target_link_libraries(can_bridge_test_channel_health PRIVATE node_health schemas capnp)
add_project_test(TARGET can_bridge_test_channel_health LABELS can_bridge unit)

# The recording tap. What is pinned here is that both directions land in the
# file with the right direction and in the recorded order, that a full queue
# drops and counts rather than blocking the thread that feeds it, and that
# hardware timestamps -- whose epoch differs per backend -- still produce
# sensible offsets.
add_executable(can_bridge_test_recorder
    test_trc_recorder.cpp
    trc_recorder.cpp
)

target_link_libraries(can_bridge_test_recorder
    PRIVATE
        can_trc
        helpers
        spdlog::spdlog
)

add_project_test(TARGET can_bridge_test_recorder LABELS can_bridge unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET can_bridge COMPONENT nodes)
