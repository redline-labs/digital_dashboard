cmake_minimum_required(VERSION 3.10)

project(map_wire)

# A road-graph segment, on the wire -- once, for everyone who publishes one.
#
# Same argument as map_rules one layer down: nodes/map_server and
# nodes/map_match both answer "what road is this", and each had its own copy of
# the enum mapping. map_common.capnp exists so the two nodes name a road class
# the same way; this exists so they DERIVE it the same way.
#
# No zenoh here, only the generated capnp types, so it stays testable without a
# bus.
add_library(map_wire STATIC
    src/segment.cpp
)

target_include_directories(map_wire PUBLIC include)

target_link_libraries(map_wire
    PUBLIC
        map_rules
        road_graph
        schemas
)

# The mapping, value by value. A test that only checked a couple of classes
# would pass while the rest reported their neighbour's -- which is the exact
# failure the switches are written out to prevent.
add_executable(map_wire_test_segment
    tests/test_segment.cpp
)

target_link_libraries(map_wire_test_segment
    PRIVATE
        map_wire
        spdlog::spdlog
)

add_project_test(TARGET map_wire_test_segment LABELS map_wire unit)
