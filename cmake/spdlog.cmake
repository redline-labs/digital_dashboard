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

FetchContent_MakeAvailable(spdlog)
