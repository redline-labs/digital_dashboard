cmake_minimum_required(VERSION 3.24)

project(csym)

# Compile-time symbolic differentiation: a residual written once as a generic
# lambda is traced into an expression graph inside constant evaluation,
# differentiated in tangent space, simplified, and lowered to straight-line
# code -- SymForce's workflow with no Python generator in the build. What it
# produces is value, Jacobian, JᵀJ and Jᵀr for a factor; the optimizer that
# consumes them is libs/factor_graph.
#
# Header-only. See docs/libs/csym.md.
add_library(csym INTERFACE)
add_library(csym::csym ALIAS csym)

target_include_directories(csym INTERFACE include)

# REQUIRED, not tuning. Graph construction runs inside constant evaluation and
# both compilers' default step budgets stop it at a few hundred nodes; a
# preintegration residual is thousands. Interface options, so every consumer
# gets them -- a factor TU without them fails with "constexpr evaluation
# exceeded", not with anything that names this file.
target_compile_options(csym INTERFACE
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-fconstexpr-steps=2000000000>
    $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=4294967296 -fconstexpr-loop-limit=2147483647>
)

function(csym_test name)
    add_executable(csym_test_${name} tests/${name}_test.cpp)
    target_include_directories(csym_test_${name} PRIVATE tests)
    target_link_libraries(csym_test_${name} PRIVATE csym ${ARGN})
    add_project_test(TARGET csym_test_${name} LABELS csym unit)
endfunction()

# The expression core, and compile-time evaluation through static_assert.
csym_test(smoke)

# Rot2/Rot3/Pose2/Pose3 Jacobians against finite differences. One TU per group:
# together they took 151 s to compile, and apart they build in parallel.
csym_test(geo_rot3)
csym_test(geo_pose2)
csym_test(geo_pose3)

# Values and tangent-space Jacobians against SymForce 0.12, run as a black box
# by tools/csym_oracle. The vectors are checked in; SymForce is not a build
# dependency.
csym_test(oracle)

# A Pacejka tire and a bicycle model: the kind of vehicle factor csym is for.
csym_test(vehicle)

# The SIMD Batch scalar type against scalar evaluation.
csym_test(batch)

# The zero-copy boundary to Eigen: csym's column-major output read through a
# Map, and analytic Jacobians computed with Eigen.
csym_test(eigen Eigen3::Eigen)

option(CSYM_BENCH "Build csym's op-count and batch benchmarks" OFF)
if(CSYM_BENCH)
    add_executable(csym_opcount bench/opcount.cpp)
    target_link_libraries(csym_opcount PRIVATE csym)
    add_executable(csym_batch_bench bench/batch_bench.cpp)
    target_link_libraries(csym_batch_bench PRIVATE csym)
endif()
