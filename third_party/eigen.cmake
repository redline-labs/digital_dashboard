# Fetch Eigen
#
# Dense and sparse linear algebra for libs/factor_graph's solve, and nothing
# else. The symbolic side (libs/csym) keeps its own constexpr Matrix: Eigen's
# fixed-size operations are not constexpr, so csym cannot trace through them,
# and its runtime output is column-major doubles that an Eigen::Map reads in
# place.
#
# 5.x rather than 3.4: 3.4 predates C++20 and warns on enum arithmetic under
# the standard this tree builds with.
set(EIGEN_GIT_TAG 5.0.1)

# Header-only, but its own CMakeLists configures BLAS/LAPACK probes, docs and
# a test suite. SOURCE_SUBDIR pointed at a directory with no CMakeLists is the
# "populate but do not configure" trick from mcap.cmake.
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG ${EIGEN_GIT_TAG}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure
)

FetchContent_MakeAvailable(eigen)

add_library(eigen INTERFACE)

# SYSTEM, unlike earcut: Eigen does not build clean under -Wconversion,
# -Wold-style-cast or -Wshadow, and the rule is that only third-party headers
# get waived. The collision earcut.cmake guards against does not arise here --
# a brewed Eigen installs under include/eigen3/, so <Eigen/...> cannot resolve
# through a bare package-manager include directory.
target_include_directories(eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})

# Refuse the few LGPL-licensed pieces at compile time, so nothing that links
# this can pick one up by accident.
target_compile_definitions(eigen INTERFACE EIGEN_MPL2_ONLY)

add_library(Eigen3::Eigen ALIAS eigen)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/eigen)
file(COPY ${eigen_SOURCE_DIR}/COPYING.MPL2 ${eigen_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/eigen)

file(WRITE ${CMAKE_BINARY_DIR}/licenses/eigen/fetch_info.txt
"Library: Eigen
Repository: https://gitlab.com/libeigen/eigen.git
Tag/Version: ${EIGEN_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
Compile Definitions: EIGEN_MPL2_ONLY
")
