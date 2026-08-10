cmake_minimum_required(VERSION 3.10)

project(msel_master_relay_node)

# Wires an MSEL Master Relay to the bus: raw CAN frames in from can_bridge,
# typed telemetry out, and its settings exposed as zenoh services.
#
# The protocol itself is libs/msel, which knows nothing about zenoh. What is
# here is translation and policy -- notably the two gates in front of remote
# shutdown, which is the one thing this node can do that stops a vehicle.
add_executable(msel_master_relay
    main.cpp
    node_config.cpp
)

target_link_libraries(msel_master_relay PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    msel
    yaml-cpp::yaml-cpp
)

# The config file is the only thing standing between a typo and a node that
# decodes nothing, or that is willing to shut the car down. Both are worth a
# test.
add_executable(msel_test_node_config
    test_node_config.cpp
    node_config.cpp
)

target_include_directories(msel_test_node_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(msel_test_node_config PRIVATE
    msel
    zenoh_pub_sub
    spdlog::spdlog
    yaml-cpp::yaml-cpp
)

add_project_test(TARGET msel_test_node_config LABELS msel unit)
