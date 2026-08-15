cmake_minimum_required(VERSION 3.10)

project(bd992_bridge)

# The BD992 node: GSOF records onto zenoh topics, and the receiver's
# configuration exposed as services.
#
# Everything protocol-shaped is in libs/gsof and libs/bd992; what is left here
# is the mapping onto capnp schemas, the YAML, and the process lifecycle.
# Named for what it does, and because `bd992` is already the library this
# links -- the same split as `can` and `can_bridge`.
add_executable(bd992_bridge
    main.cpp
    node_config.cpp
    publishers.cpp
    services.cpp
)

target_link_libraries(bd992_bridge
    PRIVATE
        bd992
        gsof
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
)

# The YAML parser, exercised without a receiver or a file. Recompiles
# node_config.cpp directly, as the can_bridge tests do, so the test drives the
# same translation unit the node does.
add_executable(bd992_test_config
    tests/test_config.cpp
    node_config.cpp
)

target_include_directories(bd992_test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(bd992_test_config
    PRIVATE
        bd992
        gsof
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET bd992_test_config LABELS bd992 unit)

# Which records describe the same instant. epoch.h is deliberately free of
# zenoh and capnp so this rule can be tested on its own -- the failures it
# guards against (a heading from one epoch against a position from another)
# produce a plausible wrong answer rather than an error, and would otherwise
# only be visible as a vehicle matched onto the road beside the one it is on.
add_executable(bd992_test_epoch
    tests/test_epoch.cpp
)

target_include_directories(bd992_test_epoch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(bd992_test_epoch
    PRIVATE
        gsof
        spdlog::spdlog
)

add_project_test(TARGET bd992_test_epoch LABELS bd992 unit)
