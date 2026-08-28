cmake_minimum_required(VERSION 3.10)

project(xpr_bridge)

# The MOTOTRBO radio node: what the radio says about itself onto topics, and
# the channel exposed as a service.
#
# Everything protocol-shaped is in libs/mototrbo and libs/xpr; what is left
# here is the mapping onto capnp schemas, the YAML, and the process lifecycle.
# Named for what it does and for the library it links, as bd992_bridge is.
add_executable(xpr_bridge
    main.cpp
    node_config.cpp
    publishers.cpp
    services.cpp
)

target_link_libraries(xpr_bridge
    PRIVATE
        xpr
        mototrbo
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
)

# The YAML parser, exercised without a radio or a file. Recompiles
# node_config.cpp directly, as the bd992 and can_bridge tests do, so the test
# drives the same translation unit the node does.
add_executable(xpr_test_config
    tests/test_config.cpp
    node_config.cpp
)

target_include_directories(xpr_test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(xpr_test_config
    PRIVATE
        xpr
        zenoh_pub_sub
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET xpr_test_config LABELS xpr unit)
