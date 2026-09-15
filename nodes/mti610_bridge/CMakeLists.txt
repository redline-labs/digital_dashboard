cmake_minimum_required(VERSION 3.10)

project(mti610_bridge)

# The MTi-610 node: MTData2 items onto zenoh topics, and the device's output
# configuration exposed as services.
#
# Everything protocol-shaped is in libs/xbus and libs/mti610; what is left here
# is the mapping onto capnp schemas, the YAML, and the process lifecycle. Named
# for what it does, and because `mti610` is already the library this links --
# the same split as `bd992` and `bd992_bridge`.
add_executable(mti610_bridge
    main.cpp
    xbus_fields.cpp
    node_config.cpp
    publishers.cpp
    services.cpp
)

target_link_libraries(mti610_bridge
    PRIVATE
    core
        mti610
        xbus
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
    node_health
)

# The YAML parser, exercised without a device or a file. Recompiles
# node_config.cpp directly, as the bd992 and can_bridge tests do, so the test
# drives the same translation unit the node does.
add_executable(mti610_test_config
    tests/test_config.cpp
    node_config.cpp
)

target_include_directories(mti610_test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mti610_test_config
    PRIVATE
        mti610
        xbus
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET mti610_test_config LABELS mti610 unit)

# One XBus item onto its schema. The axes and the units are the whole content of
# an inertial reading, so a transposed pair reads as a vehicle doing something
# else entirely.
add_executable(mti610_test_fields
    tests/test_fields.cpp
    xbus_fields.cpp
)

target_include_directories(mti610_test_fields PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mti610_test_fields
    PRIVATE
        xbus
        schemas
        capnp
        spdlog::spdlog
)

add_project_test(TARGET mti610_test_fields LABELS mti610 unit)
