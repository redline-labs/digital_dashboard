# Include FetchContent module for downloading dependencies
include(FetchContent)

# Only fetch dependencies during initial configuration, not on every build
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(FETCHCONTENT_QUIET OFF)

# Include dependency configurations
include(spdlog.cmake)
include(nlohmann_json.cmake)
include(cxxopts.cmake)
include(yaml-cpp.cmake)
include(libusb.cmake)
include(hidapi.cmake)
include(zenoh-c.cmake)
include(zenoh-cpp.cmake)
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

# LGPL Compliance: Create source code availability notice
file(COPY ${CMAKE_SOURCE_DIR}/NOTICE DESTINATION ${CMAKE_BINARY_DIR}/licenses)