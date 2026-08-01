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

# LGPL Compliance: Create source code availability notice
file(COPY ${CMAKE_SOURCE_DIR}/NOTICE DESTINATION ${CMAKE_BINARY_DIR}/licenses)