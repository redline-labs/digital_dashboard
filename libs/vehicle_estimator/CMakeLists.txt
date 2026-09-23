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
    src/clock.cpp
    src/estimator.cpp
    src/factors/dual_antenna.cpp
    src/factors/gnss_position.cpp
    src/factors/gnss_velocity.cpp
    src/factors/priors.cpp
    src/offline.cpp
)

target_include_directories(vehicle_estimator PUBLIC include)

target_link_libraries(vehicle_estimator
    PUBLIC
        imu_preint
        factor_graph
        geodesy
        Eigen3::Eigen
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
vehicle_estimator_test(factors unit)
vehicle_estimator_test(consistency slow)

# The offline smoother against the fixed-lag one on the same drive: never
# worse anywhere, and better where the forward pass is weakest.
vehicle_estimator_test(offline unit)
