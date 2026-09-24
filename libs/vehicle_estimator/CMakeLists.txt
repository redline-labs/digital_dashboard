cmake_minimum_required(VERSION 3.24)

project(vehicle_estimator)

# The vehicle state estimator: MTi strapdown increments and a dual-antenna
# GNSS receiver fused in a fixed-lag smoother on ECEF states, producing
# position, attitude, velocity, acceleration and sideslip at IMU rate. Pure
# computation -- no zenoh, no capnp -- so the node, the offline smoother and
# the tests all drive the same object. See docs/libs/vehicle_estimator.md and
# docs/design/state-estimation.md.
#
# Each measurement factor is its own translation unit: csym builds its
# residual program at compile time, and one TU per factor keeps the slowest
# of those builds off everybody else's critical path.
add_library(vehicle_estimator STATIC
    src/calibration.cpp
    src/clock.cpp
    src/estimator.cpp
    src/magnetic_reference.cpp
    src/factors/baro_height.cpp
    src/factors/calibration_prior.cpp
    src/factors/calibration_walk.cpp
    src/factors/dual_antenna.cpp
    src/factors/gnss_position.cpp
    src/factors/gnss_velocity.cpp
    src/factors/magnetometer.cpp
    src/factors/magnetometer_heading.cpp
    src/factors/priors.cpp
    src/factors/stationary_level.cpp
    src/factors/straight_driving.cpp
    src/factors/zero_velocity.cpp
    src/offline.cpp
)

target_include_directories(vehicle_estimator PUBLIC include)

target_link_libraries(vehicle_estimator
    PUBLIC
        imu_preint
        factor_graph
        geodesy
        Eigen3::Eigen
    PRIVATE
        wmm_hr2025
)

# Driving scenarios with exact truth and the sensor streams they produce.
# A library rather than test code because tools/estimator_sim writes the
# same streams into a bag.
add_library(vehicle_estimator_sim STATIC
    sim/src/scenario.cpp
)

target_include_directories(vehicle_estimator_sim PUBLIC sim/include)

target_link_libraries(vehicle_estimator_sim PUBLIC vehicle_estimator)

function(vehicle_estimator_test name)
    add_executable(vehicle_estimator_test_${name} tests/test_${name}.cpp)
    target_include_directories(vehicle_estimator_test_${name} PRIVATE tests)
    target_link_libraries(vehicle_estimator_test_${name} PRIVATE vehicle_estimator vehicle_estimator_sim spdlog::spdlog)
    add_project_test(TARGET vehicle_estimator_test_${name} LABELS vehicle_estimator ${ARGN})
endfunction()

vehicle_estimator_test(scenarios unit)
vehicle_estimator_test(calibration unit)
vehicle_estimator_test(robustness unit)
vehicle_estimator_test(clock unit)
vehicle_estimator_test(keyframes unit)

# Five minutes without GNSS: a covariance on every keyframe, sigmas that grow
# and cover the error, and a clean recovery.
vehicle_estimator_test(long_outage slow TIMEOUT 900)

# The deflection of the vertical in the Colorado foothills: the attitude tilt an
# unmodelled one leaves, and none when it is modelled.
vehicle_estimator_test(gravity slow TIMEOUT 900)
target_link_libraries(vehicle_estimator_test_gravity PRIVATE deflec core)

# What a keyframe costs the solver: iterations per keyframe on a real drive.
vehicle_estimator_test(solver unit)

# A start with no GNSS: anchored, attitude only, magnetic heading from a
# learned calibration, and one re-anchor at the first fix.
vehicle_estimator_test(cold_start slow TIMEOUT 900)
vehicle_estimator_test(factors unit)
vehicle_estimator_test(calibration_policy unit)
vehicle_estimator_test(consistency slow)

# The offline smoother against the fixed-lag one on the same drive: never
# worse anywhere, and better where the forward pass is weakest.
vehicle_estimator_test(offline unit)

# The mounting learned from straights and stops, a drift that must teach it
# nothing, and the walk following a knocked IMU. Minutes of simulated
# driving, so `slow`.
vehicle_estimator_test(mounting slow TIMEOUT 600)

# What the barometer learns while GNSS is good, and what it is worth when GNSS
# is not: a minute's outage on a hill, a fix gone autonomous, the weather.
vehicle_estimator_test(barometer slow TIMEOUT 900)

# The magnetometer's calibration learned against the antennas' heading, what
# it is worth for minutes without one, and a disturbance refused.
vehicle_estimator_test(magnetometer slow TIMEOUT 1800)
