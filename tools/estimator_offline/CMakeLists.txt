cmake_minimum_required(VERSION 3.10)

project(estimator_offline)

# The state estimator over a recorded drive: the fixed-lag estimate the car
# would have had, and the whole-drive smoothed one, written back beside the
# input. Workstation only: the batch solve holds the whole drive in memory.
add_executable(estimator_offline main.cpp)

target_link_libraries(estimator_offline
    PRIVATE
        state_estimator_core
        bag
        core
        cxxopts::cxxopts
)
