cmake_minimum_required(VERSION 3.10)

project(map_match)

# Which road the vehicle is on, and what is ahead of it.
#
# Everything algorithm-shaped is in matcher.h and horizon.h, both deliberately
# free of zenoh and capnp so the rules can be tested against a synthetic drive
# with no bus anywhere -- the same split as nodes/bd992_bridge's epoch.h. What
# is left here is the subscription, the schema mapping and the lifecycle.
add_executable(map_match
    main.cpp
    horizon.cpp
    matcher.cpp
    node_config.cpp
    services.cpp
)

target_include_directories(map_match PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(map_match
    PRIVATE
        road_graph
        map_wire
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
)

# The config surface, including the two things that make a node start cleanly
# and behave wrongly: a schema name this build does not know, and two topics
# sharing a key. Recompiles node_config.cpp directly, as map_server's test does,
# so the test drives the same translation unit the node does.
add_executable(map_match_test_config
    tests/test_config.cpp
    node_config.cpp
)

target_include_directories(map_match_test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(map_match_test_config
    PRIVATE
        zenoh_pub_sub
        schemas
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET map_match_test_config LABELS map_match unit)

# The matcher and the horizon, driven over a synthetic drive on a graph the test
# builds. What it pins is the behaviour that is silent when wrong: a heading
# that decides between parallel roads, a stationary vehicle that must not be
# re-matched on noise, and an RTK sigma that must not be treated like a
# consumer one.
add_executable(map_match_test_matcher
    tests/test_matcher.cpp
    horizon.cpp
    matcher.cpp
)

target_include_directories(map_match_test_matcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(map_match_test_matcher
    PRIVATE
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET map_match_test_matcher LABELS map_match unit)
