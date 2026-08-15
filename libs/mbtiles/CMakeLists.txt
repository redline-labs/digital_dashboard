cmake_minimum_required(VERSION 3.10)

project(mbtiles)

# The mbtiles 1.3 format, and nothing else.
#
# No zenoh, no Qt, no network, no HTTP. Everything here is answerable from a
# file on disk; nodes/map_server is what puts the answers on the bus, and
# dashboard/widgets/map never links this at all -- it asks the server.
#
# That last part is not just tidiness. Qt reaches SQLite through Qt6::Sql, which
# links its own copy; keeping this library -- and the amalgamation vendored in
# third_party/sqlite3.cmake -- out of the GUI apps keeps the two from ever
# meeting. The widget has no use for either: it asks the server.
add_library(mbtiles STATIC
    src/archive.cpp
    src/writer.cpp
    src/compression.cpp
    src/error.cpp
    src/metadata.cpp
)

target_include_directories(mbtiles PUBLIC include)

target_link_libraries(mbtiles
    PRIVATE
        sqlite3::sqlite3
        # For the TileJSON document only. The archive's own `json` column is
        # passed through rather than interpreted, so this is assembly, not
        # parsing.
        nlohmann_json::nlohmann_json
)

# spdlog is deliberately absent, for the same reason it is absent from
# libs/gsof: this library reports through Result<T>, and whether a missing tile
# is worth a log line is the server's decision, not this one's. It is the normal
# case for most of a tile pyramid.

# Reading tiles: the TMS/XYZ flip above all, plus the archive layouts and the
# concurrency the server subjects one Archive to. The tests build their own
# archives with sqlite rather than shipping a fixture -- see tests/README.md.
add_executable(mbtiles_test_archive
    tests/test_archive.cpp
)

target_include_directories(mbtiles_test_archive PRIVATE tests)

target_link_libraries(mbtiles_test_archive
    PRIVATE
        mbtiles
        # The tests WRITE archives, which the library itself never does.
        sqlite3::sqlite3
        spdlog::spdlog
)

add_project_test(TARGET mbtiles_test_archive LABELS mbtiles unit)

# The metadata table and the TileJSON built from it. Almost every field is
# optional in the spec, so this is mostly about absence and malformation.
add_executable(mbtiles_test_metadata
    tests/test_metadata.cpp
)

target_include_directories(mbtiles_test_metadata PRIVATE tests)

target_link_libraries(mbtiles_test_metadata
    PRIVATE
        mbtiles
        sqlite3::sqlite3
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

add_project_test(TARGET mbtiles_test_metadata LABELS mbtiles unit)
