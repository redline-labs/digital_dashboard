# Fetch zstd
#
# Here for MCAP chunk compression (see mcap.cmake). Chosen over lz4 as the bag
# default because its ratio/speed curve is tunable: level 1 is fast enough to
# keep up with a full bus while still roughly halving a recording, and a higher
# level can be asked for when archiving.
set(ZSTD_GIT_TAG v1.5.7)

FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG ${ZSTD_GIT_TAG}
    GIT_SHALLOW TRUE
    # zstd's CMakeLists.txt is not at the repository root, and FetchContent has
    # no way to say "configure this subdirectory" other than pointing at it.
    SOURCE_SUBDIR build/cmake
)

# Static only, and nothing but the library itself. The programs (zstd, zstdcli,
# the benchmarks) would otherwise be built and would fail the tree's -Werror
# settings, which apply to everything configured after third_party/.
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zstd)

# zstd's own CMake exports `libzstd_static` and, on newer versions, a
# `zstd::libzstd_static` alias. Normalise to one name the rest of the tree uses,
# so a version bump that changes which aliases exist is a one-line fix here
# rather than an edit in every consumer.
if(NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS libzstd_static)
endif()

# NOTE: do not add an include directory here. zstd's own CMakeLists already
# exports lib/ as `$<BUILD_INTERFACE:...>`, and adding the bare path alongside it
# makes CMake refuse to generate at all:
#
#   Target "libzstd_static" INTERFACE_INCLUDE_DIRECTORIES property contains
#   path: ".../zstd-src/lib" which is prefixed in the build directory.
#
# The generator expression is what tells CMake the path is only meaningful
# before install; a raw path claims it is valid afterwards too.

# Copy zstd license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zstd)
file(COPY ${zstd_SOURCE_DIR}/LICENSE ${zstd_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zstd)

file(WRITE ${CMAKE_BINARY_DIR}/licenses/zstd/fetch_info.txt
"Library: zstd
Repository: https://github.com/facebook/zstd.git
Tag/Version: ${ZSTD_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
")
