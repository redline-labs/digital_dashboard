cmake_minimum_required(VERSION 3.24)

project(imu_preint)

# IMU preintegration from strapdown increments, and the two factors the IMU
# puts in the graph: the preintegrated interval between keyframes (in ECEF,
# with the earth's rotation) and the bias random walk. Also the sample
# sequencer for an MTi's wrapping counters, and a truth simulator that turns
# a trajectory into the increments an ideal IMU would report. Sensor-agnostic
# beyond "gives dq and dv". See docs/libs/imu_preint.md.
add_library(imu_preint STATIC
    src/preintegrator.cpp
    src/imu_factor.cpp
    src/sequencer.cpp
    src/sim.cpp
)

target_include_directories(imu_preint PUBLIC include)

target_link_libraries(imu_preint
    PUBLIC
        factor_graph
        geodesy
        Eigen3::Eigen
)

function(imu_preint_test name)
    add_executable(imu_preint_test_${name} tests/test_${name}.cpp)
    target_include_directories(imu_preint_test_${name} PRIVATE tests)
    target_link_libraries(imu_preint_test_${name} PRIVATE imu_preint spdlog::spdlog)
    add_project_test(TARGET imu_preint_test_${name} LABELS imu_preint ${ARGN})
endfunction()

# Bias Jacobians against re-integration, splitting
# an increment at a keyframe, and every increment that must be refused.
imu_preint_test(preintegrator unit)

# The ECEF model against truth built in the inertial frame, what the earth
# rate and the dv frame are each worth, the factor's Jacobians, and a solve
# that recovers both biases.
imu_preint_test(imu unit)

# Counter and tick wraps, gaps, duplicates, and a gap too long to bridge.
imu_preint_test(sequencer unit)

# The propagated covariance against 4000 noisy re-integrations.
imu_preint_test(covariance slow)
