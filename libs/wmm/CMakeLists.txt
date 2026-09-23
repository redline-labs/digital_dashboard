cmake_minimum_required(VERSION 3.24)

project(wmm)

# The World Magnetic Model: a compile-time parser for NOAA/NCEI .COF files and
# the field synthesis, ported from NCEI's GeomagnetismLibrary.c. What the
# estimator wants from it is the reference field vector at a position and
# date, which a magnetometer factor compares the MTi's reading against.
#
# `wmm` is the generic header-only part. `wmm_hr2025` is WMMHR-2025 (degree
# 133) behind plain functions: its one TU #embeds the coefficient file and
# parses it during compilation, so a damaged file is a build error naming the
# line, nothing is read at runtime, and consumers never see the parser.
# See docs/libs/wmm.md.
add_library(wmm INTERFACE)
target_include_directories(wmm INTERFACE include)
target_link_libraries(wmm INTERFACE geodesy)

add_library(wmm_hr2025 STATIC src/wmmhr2025.cpp)
target_include_directories(wmm_hr2025 PUBLIC include)
target_link_libraries(wmm_hr2025 PRIVATE wmm)

# The twelve NCEI test points, every column, each a static_assert: a wrong
# coefficient or a wrong recursion fails the build. Also checks compile-time
# and runtime evaluation agree.
add_executable(wmm_test_hr2025 tests/test_wmmhr.cpp)
target_include_directories(wmm_test_hr2025 PRIVATE tests)
target_link_libraries(wmm_test_hr2025 PRIVATE wmm)
add_project_test(TARGET wmm_test_hr2025 LABELS wmm unit)

# The parser: every error it reports and the line it blames, malformed files,
# and a runtime parse of the full file against the compiled library's model.
add_executable(wmm_test_cof tests/test_cof.cpp)
target_include_directories(wmm_test_cof PRIVATE tests)
target_link_libraries(wmm_test_cof PRIVATE wmm wmm_hr2025)
add_project_test(TARGET wmm_test_cof LABELS wmm unit)

# The compiled interface, from a TU that must not be able to reach the parser.
add_executable(wmm_test_api tests/test_api.cpp)
target_link_libraries(wmm_test_api PRIVATE wmm_hr2025)
add_project_test(TARGET wmm_test_api LABELS wmm unit)
