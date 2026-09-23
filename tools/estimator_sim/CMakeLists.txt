cmake_minimum_required(VERSION 3.10)

project(estimator_sim)

# A simulated drive recorded as a bag: the two bridges' topics as they would
# have been published, plus the truth on sim/truth. What drives the live
# state_estimator (through `bag play`) and estimator_offline without a car.
add_executable(estimator_sim main.cpp)

target_link_libraries(estimator_sim
    PRIVATE
        state_estimator_sim_bus
        bag
        core
        cxxopts::cxxopts
)
