# Patches resolve against THIS file, not CMAKE_SOURCE_DIR -- that is the top of
# whatever project is configuring, so a consumer including this file from its
# own tree got "can't open patch '<their root>/patches/...'".
get_filename_component(REDLINE_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../patches" ABSOLUTE)

# Fetch spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    PATCH_COMMAND git apply ${REDLINE_PATCH_DIR}/spdlog_tweakme.patch
)

# Configure spdlog options before making it available
# Disable building tests, examples, and benchmarks
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spdlog)

# Copy spdlog license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/spdlog)
file(COPY ${spdlog_SOURCE_DIR}/LICENSE ${spdlog_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/spdlog)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/spdlog/fetch_info.txt
"Library: spdlog
Repository: https://github.com/gabime/spdlog.git
Tag/Version: v1.17.0
Shallow Clone: TRUE
Patches Applied: spdlog_tweakme.patch
")

# Copy spdlog patches
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/spdlog/patches)
file(COPY ${REDLINE_PATCH_DIR}/spdlog_tweakme.patch
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/spdlog/patches)
