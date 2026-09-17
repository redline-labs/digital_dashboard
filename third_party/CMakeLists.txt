# Include FetchContent module for downloading dependencies
include(FetchContent)

# Only fetch dependencies during initial configuration, not on every build
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(FETCHCONTENT_QUIET OFF)

# No vendored dependency's test suite, ever, whatever our own BUILD_TESTING says.
# Directory-scoped, so it reaches everything below (including the sub-projects
# FetchContent adds) without touching the cache entry that decides OUR tests.
# It used to be a forced cache write in capnproto.cmake, which is how one
# vendored dependency ended up deciding whether the project had tests.
set(BUILD_TESTING OFF)

# Include dependency configurations
include(spdlog.cmake)
include(nlohmann_json.cmake)
include(cxxopts.cmake)
include(yaml-cpp.cmake)
include(libusb.cmake)
include(hidapi.cmake)
include(zenoh-c.cmake)
include(zenoh-cpp.cmake)
# The router, built from the same patched workspace as the library above.
# Off unless REDLINE_BUILD_ZENOHD=ON; the image turns it on.
include(zenohd.cmake)
include(capnproto.cmake)
include(exprtk.cmake)
include(lexy.cmake)

# The bag format and its compression codecs. mcap.cmake links the two codec
# targets, so they have to be defined first.
include(zstd.cmake)
include(lz4.cmake)
include(mcap.cmake)

# What an .mbtiles archive is made of. Used by libs/mbtiles, and so by
# nodes/map_server; the map widget never links it, because it asks the node.
include(sqlite3.cmake)

# Polygon triangulation for the map widget GPU renderer. See earcut.cmake for
# why this is not hand-rolled.
include(earcut.cmake)

# The web console's HTTP server. Header-only; see the file for why it is not
# a SYSTEM include and where the warning suppression actually lives.
include(cpp-httplib.cmake)

# LGPL Compliance: Create source code availability notice
file(COPY ${CMAKE_CURRENT_LIST_DIR}/../NOTICE DESTINATION ${CMAKE_BINARY_DIR}/licenses)