cmake_minimum_required(VERSION 3.10)

project(map_build)

# An OSM PBF in; the road graph out (and, from stage 5, the tiles).
#
# Thin on purpose. Everything format-shaped is in libs/osm, everything rule-
# shaped in libs/map_rules, everything layout-shaped in libs/road_graph; what is
# left here is the three-pass loop that joins them and the CLI. The same split
# as nodes/map_server against libs/mbtiles.
add_executable(map_build
    main.cpp
    extract.cpp
    graph.cpp
    overlay.cpp
    rings.cpp
    route.cpp
    tile.cpp
    tiler.cpp
    track_geometry.cpp
    track_source.cpp
    tracks.cpp
    verify.cpp
)

target_include_directories(map_build PRIVATE include)

# libs/cli brings zenoh_pub_sub with it, which this tool never uses. That is a
# link-time cost on a workstation binary and buys the verb table, the help text
# and the 0/1/2 exit codes -- worth it here, and stated so nobody removes the
# dependency wondering why a build tool links a bus.
target_link_libraries(map_build
    PRIVATE
        cli
        map_rules
        mbtiles
        mvt
        nlohmann_json::nlohmann_json
        osm
        road_graph
        # The race-track catalogue, which the `tracks` verb writes into the same
        # .mbtiles it just filled with tiles.
        track_store
        spdlog::spdlog
        cxxopts::cxxopts
)

# The extraction rules, end to end: a PBF built in the test, through classify(),
# into a graph, and back out. What it pins is the SPLITTING -- a way becomes
# segments at its junctions and nowhere else -- and the dangling-reference
# policy, both of which are silent when wrong.
add_executable(map_build_test_extract
    tests/test_extract.cpp
    extract.cpp
    rings.cpp
)

# libs/osm's own builder writes PBF bytes longhand from the spec; reusing it
# here keeps one hand-written encoder rather than two that could drift.
target_include_directories(map_build_test_extract
    PRIVATE include tests ${CMAKE_SOURCE_DIR}/libs/osm/tests)

target_link_libraries(map_build_test_extract
    PRIVATE
        map_rules
        osm
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET map_build_test_extract LABELS map_build unit)

# Projection, simplification and clipping -- the three pieces of tile geometry
# whose failures RENDER. A tile built with the wrong tolerance draws roads that
# wander off their junctions; one built without a buffer draws a seam down every
# boundary; one whose polygons are line-clipped leaves fills open.
add_executable(map_build_test_tiler
    tests/test_tiler.cpp
    tiler.cpp
)

target_include_directories(map_build_test_tiler PRIVATE include tests)

target_link_libraries(map_build_test_tiler
    PRIVATE
        map_rules
        mbtiles
        mvt
        nlohmann_json::nlohmann_json
        osm
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET map_build_test_tiler LABELS map_build unit)

# Stitching a multipolygon's member ways into rings is graph work, not geometry
# work, so it is tested on a handful of arcs given in a deliberately awkward
# order -- shuffled, half of them reversed -- with no PBF anywhere near it.
add_executable(map_build_test_rings
    tests/test_rings.cpp
    rings.cpp
)

target_include_directories(map_build_test_rings PRIVATE include)

target_link_libraries(map_build_test_rings
    PRIVATE
        osm
        spdlog::spdlog
)

add_project_test(TARGET map_build_test_rings LABELS map_build unit)

# Splitting a traced track OUTLINE into its two loops and averaging them into a
# centreline. The fixture is an annulus, whose answers are known in closed form,
# built at a latitude where forgetting the projection is visible.
#
# Every failure this guards against is SILENT: a reflected pairing preserves
# length, a file holding four loops yields a plausible-looking width, and a
# source whose outline disagrees with its own published lap reads as our
# arithmetic going wrong rather than as bad data.
add_executable(map_build_test_track_geometry
    tests/test_track_geometry.cpp
    track_geometry.cpp
)

target_include_directories(map_build_test_track_geometry PRIVATE include)

target_link_libraries(map_build_test_track_geometry
    PRIVATE
        osm
        road_graph
        spdlog::spdlog
)

add_project_test(TARGET map_build_test_track_geometry LABELS map_build unit)
