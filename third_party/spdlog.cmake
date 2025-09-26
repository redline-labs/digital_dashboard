# Fetch spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG 6fa3601  # Version 1.15.3
    GIT_SHALLOW TRUE
    PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/patches/spdlog_tweakme.patch
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
Tag/Version: 6fa3601 (Version 1.15.3)
Shallow Clone: TRUE
Patches Applied: spdlog_tweakme.patch
")

# Copy spdlog patches
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/spdlog/patches)
file(COPY ${CMAKE_SOURCE_DIR}/patches/spdlog_tweakme.patch
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/spdlog/patches)