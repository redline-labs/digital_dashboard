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
include(zenoh-c.cmake)
include(zenoh-cpp.cmake)

# Create a common logging interface to avoid duplicate spdlog linking
add_library(common_logging INTERFACE)
target_link_libraries(common_logging INTERFACE spdlog::spdlog)

# LGPL Compliance: Create source code availability notice
file(COPY ${CMAKE_SOURCE_DIR}/NOTICE DESTINATION ${CMAKE_BINARY_DIR}/licenses)