cmake_minimum_required(VERSION 3.10)

project(state_estimator)

# The vehicle state estimator on the bus: the MTi's strapdown increments and
# the BD992's GSOF records in, position, attitude, velocity, acceleration and
# sideslip out at the IMU's rate.
#
# Everything with an answer that can be wrong is outside main.cpp and outside
# zenoh: the estimation in libs/vehicle_estimator, the record pairing in
# gnss_assembler and imu_assembler, the decoding in decode.cpp, and all of it
# driven through Pipeline -- the same object the node, --replay and the tests
# run. What is left here is subscriptions, threads and lifecycle.
add_library(state_estimator_core STATIC
    calibration_keeper.cpp
    decode.cpp
    gnss_assembler.cpp
    imu_assembler.cpp
    node_config.cpp
    pipeline.cpp
    state_fields.cpp
)

target_include_directories(state_estimator_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(state_estimator_core
    PUBLIC
        vehicle_estimator
        calibration_store
        core
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

add_executable(state_estimator main.cpp)

target_link_libraries(state_estimator
    PRIVATE
        state_estimator_core
        core
        bag
        node_health
        cxxopts::cxxopts
)

# A simulated drive as the bridges' capnp messages. Test and tool support, not
# shipped: it links the simulator.
add_library(state_estimator_sim_bus STATIC sim_bus.cpp)
target_link_libraries(state_estimator_sim_bus PUBLIC state_estimator_core vehicle_estimator_sim)

function(state_estimator_test name)
    add_executable(state_estimator_test_${name} tests/test_${name}.cpp)
    target_link_libraries(state_estimator_test_${name} PRIVATE state_estimator_core ${ARGN})
    add_project_test(TARGET state_estimator_test_${name} LABELS state_estimator unit)
endfunction()

# Pairing the per-record GSOF topics: bursts, the time each epoch gets, the
# slow records used by age, and what is dropped rather than guessed.
state_estimator_test(gnss_assembler)

# delta_q and delta_v back into one sample, by packet counter.
state_estimator_test(imu_assembler)

# The config surface, including every value that would make the node start
# and estimate wrongly.
state_estimator_test(config)
# Reads the shipped configs/state_estimator/state_estimator.yaml.
target_compile_definitions(state_estimator_test_config PRIVATE REDLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}/../..")

# A simulated drive as capnp messages -- the bytes the bridges would put on
# the bus -- through decode, pairing and the estimator.
state_estimator_test(pipeline state_estimator_sim_bus)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET state_estimator COMPONENT nodes)

# What was learned, kept between sessions in a real SQLite file: five
# simulated drives through Pipeline and CalibrationKeeper. Minutes of driving,
# so `slow`.
add_executable(state_estimator_test_calibration_persistence tests/test_calibration_persistence.cpp)
target_link_libraries(state_estimator_test_calibration_persistence PRIVATE state_estimator_core state_estimator_sim_bus)
add_project_test(TARGET state_estimator_test_calibration_persistence LABELS state_estimator slow TIMEOUT 900)
