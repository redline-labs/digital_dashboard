cmake_minimum_required(VERSION 3.10)

project(osm)

# OpenStreetMap PBF, decoded.
#
# No Qt, no zenoh, no I/O beyond a span of bytes -- the same split as libs/mvt
# and libs/gsof, and for the same reason: everything here is reachable from a
# buffer, so all of it is testable without a 637 MB file.
#
# The split INSIDE the library matters as much: blob.h walks framing and yields
# still-compressed spans, block.h turns one such span into entities as a pure
# function. That is what lets tools/map_build inflate and parse on every core
# while the framing walk stays sequential. Fusing them would bake in
# single-threaded, and at continental scale that is the difference between a
# five-minute pass and a twenty-minute one.
add_library(osm STATIC
    src/blob.cpp
    src/block.cpp
    src/error.cpp
    src/node_store.cpp
)

target_include_directories(osm PUBLIC include)

# zlib for inflate, exactly as libs/mvt does it. PBF blobs are zlib-wrapped
# (window bits 15) rather than gzip, so this does not go through mvt/gzip.h --
# which auto-detects, because tiles vary and a PBF does not.
find_package(ZLIB REQUIRED)

target_link_libraries(osm
    PUBLIC
        protowire
    PRIVATE
        ZLIB::ZLIB
)

# No spdlog: this reports through Result<T>, and whether one bad block in a
# continental file is worth a log line is map_build's decision, not this one's.

# The framing. Every case here is one where a plausible reader keeps going and
# produces garbage: a big-endian length read little-endian, a size cap that is
# not enforced, a codec that is silently treated as empty.
add_executable(osm_test_blob
    tests/test_blob.cpp
)

target_include_directories(osm_test_blob PRIVATE tests)

target_link_libraries(osm_test_blob
    PRIVATE
        osm
        spdlog::spdlog
        ZLIB::ZLIB
)

add_project_test(TARGET osm_test_blob LABELS osm unit)

# The block grammar, and the five traps in it that render rather than fail.
# tests/pbf_builder.h encodes longhand from the spec rather than through this
# library's own primitives, so a test cannot agree with the bug it checks for --
# the same argument as libs/mvt/tests/tile_builder.h.
add_executable(osm_test_block
    tests/test_block.cpp
)

target_include_directories(osm_test_block PRIVATE tests)

target_link_libraries(osm_test_block
    PRIVATE
        osm
        spdlog::spdlog
)

add_project_test(TARGET osm_test_block LABELS osm unit)

# bitset -> rank -> dense array, and the ordering check that guards it.
# The cases that matter are the boundaries: id 0, a superblock edge, and an id
# that was referenced but never resolved.
add_executable(osm_test_node_store
    tests/test_node_store.cpp
)

target_link_libraries(osm_test_node_store
    PRIVATE
        osm
        spdlog::spdlog
)

add_project_test(TARGET osm_test_node_store LABELS osm unit)

# The reader against a real extract, if one is present. Two full passes over
# 637 MB, so it is labelled `slow` and stays out of `ctest -LE slow`; it SKIPS
# loudly when the file is absent, exactly as mvt_test_real_tiles does, because
# the file is not in the repository and a fresh checkout must still pass.
add_executable(osm_test_real_extract
    tests/test_real_extract.cpp
)

target_link_libraries(osm_test_real_extract
    PRIVATE
        osm
        spdlog::spdlog
)

add_project_test(TARGET osm_test_real_extract LABELS osm slow TIMEOUT 600)
