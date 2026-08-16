cmake_minimum_required(VERSION 3.10)

project(map_server)

# The map node: .mbtiles archives on zenoh as tiles, catalogs and style assets.
#
# Everything format-shaped is in libs/mbtiles; what is left here is the mapping
# onto capnp schemas, the YAML, and the process lifecycle. The same split as
# bd992_bridge against libs/bd992, and named for what it does rather than what
# it reads.
#
# Note what is NOT here: anything that draws. The renderer lives in
# dashboard/widgets/map and reaches this node over the bus, which is the whole
# point -- and it is also what keeps the sqlite3 this links out of the GUI apps.
add_executable(map_server
    main.cpp
    asset_store.cpp
    graphs.cpp
    node_config.cpp
    route_endpoints.cpp
    services.cpp
    tilesets.cpp
)

target_link_libraries(map_server
    PRIVATE
        mbtiles
        road_graph
        map_wire
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
)

# The config surface: the defaults, and the two collisions that make a server
# start cleanly and answer wrongly -- a duplicate tileset name, and two services
# sharing a key. Recompiles node_config.cpp directly, as bd992_bridge and
# can_bridge do, so the test drives the same translation unit the node does.
add_executable(map_server_test_config
    tests/test_config.cpp
    node_config.cpp
)

target_include_directories(map_server_test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(map_server_test_config
    PRIVATE
        zenoh_pub_sub
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET map_server_test_config LABELS map unit)

# The containment check. This is the only code in the node that turns text off
# the bus into a filesystem path, and the case worth the test is the sibling
# directory sharing a name prefix -- "/maps/assets-evil" passes a string-prefix
# containment check and fails a component-wise one.
add_executable(map_server_test_assets
    tests/test_assets.cpp
    asset_store.cpp
)

target_include_directories(map_server_test_assets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(map_server_test_assets
    PRIVATE
        mbtiles
        spdlog::spdlog
)

add_project_test(TARGET map_server_test_assets LABELS map unit)

# Which profiles a graph offers, and which it accepts -- the two answers that
# used to come from different places and disagree. Recompiles graphs.cpp
# directly, as the config test does with node_config.cpp, so the test drives
# the same translation unit the node does.
add_executable(map_server_test_graphs
    tests/test_graphs.cpp
    graphs.cpp
)

target_include_directories(map_server_test_graphs PRIVATE .)

target_link_libraries(map_server_test_graphs
    PRIVATE
        road_graph
        map_rules
        zenoh_pub_sub
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_project_test(TARGET map_server_test_graphs LABELS map unit)

# Which junctions a route search runs between. The departure heading was being
# used to rank candidates and then discarded, so a vehicle pointing against its
# segment's stored direction was routed from the junction behind it.
add_executable(map_server_test_route_endpoints
    tests/test_route_endpoints.cpp
    route_endpoints.cpp
)

target_include_directories(map_server_test_route_endpoints PRIVATE .)

target_link_libraries(map_server_test_route_endpoints
    PRIVATE
        road_graph
        map_rules
        spdlog::spdlog
)

add_project_test(TARGET map_server_test_route_endpoints LABELS map unit)
