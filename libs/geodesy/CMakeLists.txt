cmake_minimum_required(VERSION 3.24)

project(geodesy)

# The WGS 84 ellipsoid: geodetic <-> ECEF, the local NED frame, radii of
# curvature, and normal gravity. Header-only and branch-free templates over
# the scalar, so a csym factor can differentiate straight through them; the
# math comes from csym's front-ends for the same reason. See
# docs/libs/geodesy.md.
add_library(geodesy INTERFACE)

target_include_directories(geodesy INTERFACE include)

target_link_libraries(geodesy INTERFACE csym)

# Round trip, an independent inverse (Bowring against Vermeille), the NED
# frame's geometry, input rejection, and the traced Jacobian.
add_executable(geodesy_test_geodetic tests/test_geodetic.cpp)
target_link_libraries(geodesy_test_geodetic PRIVATE geodesy spdlog::spdlog)
add_project_test(TARGET geodesy_test_geodetic LABELS geodesy unit)

# Normal gravity against its published values and against a zonal potential
# (J2..J8) built from different physics.
add_executable(geodesy_test_gravity tests/test_gravity.cpp)
target_link_libraries(geodesy_test_gravity PRIVATE geodesy spdlog::spdlog)
add_project_test(TARGET geodesy_test_gravity LABELS geodesy unit)
