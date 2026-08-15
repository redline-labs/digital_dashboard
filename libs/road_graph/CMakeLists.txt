cmake_minimum_required(VERSION 3.10)

project(road_graph)

# The routable road network, on disk and mmap'd.
#
# format.h states the six decisions a later stage cannot undo -- two identity
# levels, ids that are not array indices, the way index, geometry stored once,
# Hilbert ordering, an extensible section table -- each where it is made.
#
# Read side and build side are the same library because they share the record
# layouts and nothing else; the build side is used only by tools/map_build,
# which never ships, and the read side by nodes/map_server and nodes/map_match,
# which do. No zenoh and no Qt: a graph is a file, and everything about it is
# testable from one.
add_library(road_graph STATIC
    src/builder.cpp
    src/error.cpp
    src/graph.cpp
    src/contraction.cpp
    src/overlay.cpp
    src/search.cpp
)

target_include_directories(road_graph PUBLIC include)

# The classification a segment carries comes straight from map_rules, so the
# builder takes it rather than re-deriving it. That is the anti-divergence rule
# expressed as a dependency.
target_link_libraries(road_graph PUBLIC map_rules PRIVATE spdlog::spdlog)

# Build a graph, read it back, and check the things that are silently wrong if
# they break: an id that does not survive a rebuild, geometry that is duplicated
# rather than shared, a way whose pieces are not contiguous, a spatial query
# that misses.
add_executable(road_graph_test_format
    tests/test_format.cpp
)

target_link_libraries(road_graph_test_format
    PRIVATE
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET road_graph_test_format LABELS road_graph unit)

# Nearest-segment search, which is what map/nearest and the matcher both stand
# on. The case that matters is the one distance alone gets wrong: a road beside
# a road, told apart by heading.
add_executable(road_graph_test_spatial
    tests/test_spatial.cpp
)

target_link_libraries(road_graph_test_spatial
    PRIVATE
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET road_graph_test_spatial LABELS road_graph unit)

# Routing, and the three rules that make a route one a driver can follow:
# oneway, turn restrictions and U-turns. Each is tested by building a graph
# where the honest answer is the LONGER way round -- a router that ignored any
# of them would still return a route, a shorter one, that nobody can drive.
add_executable(road_graph_test_routing
    tests/test_routing.cpp
)

target_link_libraries(road_graph_test_routing
    PRIVATE
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET road_graph_test_routing LABELS road_graph unit)

# The hierarchy, checked against the router it replaces. A wrong hierarchy does
# not fail -- it answers quickly and slightly wrong -- so the assertion is that
# every pair costs EXACTLY what plain A* found.
add_executable(road_graph_test_contraction
    tests/test_contraction.cpp
)

target_link_libraries(road_graph_test_contraction
    PRIVATE
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET road_graph_test_contraction LABELS road_graph unit)
