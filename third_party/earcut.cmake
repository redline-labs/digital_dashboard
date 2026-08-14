# Fetch earcut.hpp
#
# Polygon triangulation, for turning MVT fill rings into the triangles the GPU
# actually draws. One header, ISC licensed, and Mapbox's own implementation --
# so it is exercised against real vector tiles far harder than anything we would
# write.
#
# Vendored rather than hand-rolled because ear clipping WITH HOLES is one of those
# algorithms that is easy to get almost right: the failure is a lake that fills
# over its own island, or a spike across a park, and it only shows up on the one
# concave polygon nobody screenshotted. A fan triangulation -- the obvious
# shortcut -- gets every concave shape wrong.
set(EARCUT_GIT_TAG v3.2.3)

# Header-only, but its own CMakeLists builds tests, benchmarks and a viewer and
# pulls dependencies to do it. SOURCE_SUBDIR pointed at a directory with no
# CMakeLists is the "populate but do not configure" trick from mcap.cmake.
FetchContent_Declare(
    earcut
    GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
    GIT_TAG ${EARCUT_GIT_TAG}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR include
)

FetchContent_MakeAvailable(earcut)

add_library(earcut INTERFACE)

# NOT a SYSTEM include: `mapbox/earcut.hpp` is not a Homebrew formula today, but
# the root CMakeLists documents what an -isystem entry costs when it ever
# becomes one, and a plain include cannot lose that race.
target_include_directories(earcut INTERFACE ${earcut_SOURCE_DIR}/include)

add_library(earcut::earcut ALIAS earcut)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/earcut)
file(COPY ${earcut_SOURCE_DIR}/LICENSE ${earcut_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/earcut)

file(WRITE ${CMAKE_BINARY_DIR}/licenses/earcut/fetch_info.txt
"Library: earcut.hpp
Repository: https://github.com/mapbox/earcut.hpp.git
Tag/Version: ${EARCUT_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
")
