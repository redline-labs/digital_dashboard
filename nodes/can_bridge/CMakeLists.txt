cmake_minimum_required(VERSION 3.10)

project(can_bridge)

add_executable(can_bridge
    main.cpp
    node_config.cpp
)

target_link_libraries(can_bridge
    PRIVATE
        can
        can_backends
        zenoh_pub_sub
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
        capnp
        schemas
        helpers
)

# The config surface: what an absent key defaults to, and the collisions that
# would make two channels fight over one adapter or one topic.
add_executable(can_bridge_test_config
    test_node_config.cpp
    node_config.cpp
)

target_link_libraries(can_bridge_test_config
    PRIVATE
        can
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET can_bridge_test_config LABELS can unit)
