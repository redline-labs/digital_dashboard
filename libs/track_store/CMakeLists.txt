cmake_minimum_required(VERSION 3.10)

project(track_store)

# The race-track catalogue, which lives inside the .mbtiles beside the tiles.
#
# No zenoh, no Qt, no network. Everything here is answerable from a file on
# disk; nodes/map_server is what puts the answers on the bus, and
# dashboard/widgets/map never links this at all -- it asks the server.
#
# That last part is the same rule libs/mbtiles states and for the same reason:
# Qt reaches SQLite through Qt6::Sql with its own copy, and keeping this library
# -- and the amalgamation in third_party/sqlite3.cmake -- out of the GUI apps
# keeps the two from ever meeting.
#
# spdlog is deliberately absent: this reports through Result<T>, and whether a
# track without a centreline is worth a log line is the caller's decision. It is
# the normal case for a fifth of the corpus.
add_library(track_store STATIC
    src/store.cpp
)

target_include_directories(track_store PUBLIC include)

target_link_libraries(track_store
    PRIVATE
        sqlite3::sqlite3
)

# What the one-artifact claim rests on: extra tables must be invisible to
# mbtiles::Archive, and a catalogue whose build id disagrees with the tiles'
# must be REFUSED rather than served. Both failures are silent -- the archive
# opens, the tiles draw, and a lap distance is measured against a centreline
# from a different ingest run.
#
# The test builds its own archive rather than shipping a fixture, as
# mbtiles_test_archive does.
add_executable(track_store_test_store
    tests/test_store.cpp
)

target_link_libraries(track_store_test_store
    PRIVATE
        track_store
        mbtiles
        sqlite3::sqlite3
        spdlog::spdlog
)

add_project_test(TARGET track_store_test_store LABELS track_store unit)
