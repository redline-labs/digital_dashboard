cmake_minimum_required(VERSION 3.10)

project(mvt)

# Mapbox Vector Tiles, decoded.
#
# No Qt, no zenoh, no I/O. Bytes in, geometry and attributes out -- the same
# split as libs/gsof against libs/bd992, and for the same reason: everything
# here is reachable from a buffer, so all of it is testable without a display or
# a bus, against real tiles pulled out of a real archive.
#
# This exists because the renderer is ours, and the renderer is ours because
# every off-the-shelf Qt one binds to a platform surface and so goes blank under
# QT_QPA_PLATFORM=offscreen -- which would leave the map invisible to
# ui_screenshot and to every `gui` test. A vector tile is protobuf with a
# command-encoded geometry stream; that turned out to be a smaller thing to own
# than the consequences of not owning it. See docs/map.md.
add_library(mvt STATIC
    src/decode.cpp
    src/encode.cpp
    src/gzip.cpp
    src/tile.cpp
)

target_include_directories(mvt PUBLIC include)

# PUBLIC, not PRIVATE: mvt/error.h and mvt/reader.h are alias headers that
# re-export protowire's types, so every consumer of an mvt Result needs
# protowire's include path too.
target_link_libraries(mvt PUBLIC protowire)

# zlib for inflate, and nothing else. Tiles are stored gzipped and travel that
# way; see mvt/gzip.h. Present on macOS and every Linux, so found rather than
# vendored -- and NOT marked SYSTEM, because zlib is also a Homebrew formula and
# the root CMakeLists documents what an -isystem collision costs.
find_package(ZLIB REQUIRED)

target_link_libraries(mvt PRIVATE ZLIB::ZLIB)

# spdlog is deliberately absent, as in libs/gsof: this library reports through
# Result<T>, and whether one bad tile in a viewport of forty is worth a log line
# is the widget's decision, not this one's.

# The wire format. Everything here is a way the format is easy to get subtly
# wrong -- a ring that winds the other way, an extent that is not 4096, a
# coordinate outside the tile -- and every one of them renders a map that looks
# fine and is wrong, rather than failing.
add_executable(mvt_test_decode
    tests/test_decode.cpp
)

target_include_directories(mvt_test_decode PRIVATE tests)

target_link_libraries(mvt_test_decode
    PRIVATE
        mvt
        spdlog::spdlog
)

add_project_test(TARGET mvt_test_decode LABELS mvt unit)

# Decoding REAL tiles, from the archive in configs/map_server.yaml if it is
# present. Skips itself when it is not, so a checkout without the 383 MB file
# still passes -- but on a machine that has it, this is the only test that
# proves the decoder against bytes nobody here wrote.
add_executable(mvt_test_real_tiles
    tests/test_real_tiles.cpp
)

target_link_libraries(mvt_test_real_tiles
    PRIVATE
        mvt
        mbtiles
        spdlog::spdlog
)

add_project_test(TARGET mvt_test_real_tiles LABELS mvt unit)

# Encode, then decode, and require the two to agree.
#
# The round trip is the strongest test either direction has: an encoder bug that
# a decoder mirrors would pass a one-sided test and produce tiles no other
# renderer could read. Every trap decode.h names is asserted from the writing
# side here -- the persistent cursor, the implied closing point, ring winding.
add_executable(mvt_test_encode
    tests/test_encode.cpp
)

target_include_directories(mvt_test_encode PRIVATE tests)

target_link_libraries(mvt_test_encode
    PRIVATE
        mvt
        spdlog::spdlog
)

add_project_test(TARGET mvt_test_encode LABELS mvt unit)
