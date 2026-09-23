cmake_minimum_required(VERSION 3.24)

project(factor_graph)

# A nonlinear least-squares factor graph and the two smoothers the state
# estimator runs on it: a fixed-lag smoother for the car, a batch smoother
# for a drive after the fact. Residuals come from csym, the linear algebra
# from Eigen. Nothing here knows what a sensor is. See
# docs/libs/factor_graph.md.
add_library(factor_graph STATIC
    src/factor.cpp
    src/linear_prior.cpp
    src/optimizer.cpp
    src/smoother.cpp
)

target_include_directories(factor_graph PUBLIC include)

# spdlog only for its bundled fmt, which formats the refusal messages; the
# library logs nothing -- whether a refused factor is worth a line is the
# node's call.
target_link_libraries(factor_graph
    PUBLIC
        csym
        Eigen3::Eigen
    PRIVATE
        spdlog::spdlog
)

# The oracles. A linear-Gaussian chain, where the fixed-lag smoother must
# reproduce a Kalman filter and the batch smoother an RTS smoother -- to
# rounding, not approximately -- because marginalisation is exact there.
add_executable(factor_graph_test_kalman tests/test_kalman.cpp)
target_link_libraries(factor_graph_test_kalman PRIVATE factor_graph spdlog::spdlog)
add_project_test(TARGET factor_graph_test_kalman LABELS factor_graph unit)

# The machinery on nonlinear problems: manifold variables, the marginal
# prior's Jacobian, LM step control, covariance of what cannot be observed,
# and every way an update can be malformed.
add_executable(factor_graph_test_graph tests/test_graph.cpp)
target_link_libraries(factor_graph_test_graph PRIVATE factor_graph spdlog::spdlog)
add_project_test(TARGET factor_graph_test_graph LABELS factor_graph unit)
