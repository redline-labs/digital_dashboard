cmake_minimum_required(VERSION 3.10)

project(protowire)

# Just enough protobuf, for the formats in this tree that are protobuf.
#
# Mapbox Vector Tiles (libs/mvt) and OpenStreetMap PBF (libs/osm). This was
# libs/mvt's reader until libs/osm needed it too, at which point leaving it
# there would have made an OSM extractor depend on a vector-tile library.
#
# libs/mvt re-exports every name from here into namespace mvt, so the move cost
# no call-site edits -- and protowire_test_reader is the same test file that
# used to be mvt_test_reader, required to pass unchanged.
add_library(protowire STATIC
    src/error.cpp
    src/reader.cpp
)

target_include_directories(protowire PUBLIC include)

# No spdlog, as in libs/mvt and libs/gsof: this reports through Result<T>, and
# whether one bad message in a batch is worth a log line is the caller's
# decision, not this one's.

# Varints, zigzag, wire types, and the truncations that must be errors rather
# than plausible numbers. Every case here is one where continuing would yield a
# number rather than a failure.
add_executable(protowire_test_reader
    tests/test_reader.cpp
)

target_link_libraries(protowire_test_reader
    PRIVATE
        protowire
        spdlog::spdlog
)

add_project_test(TARGET protowire_test_reader LABELS protowire unit)
