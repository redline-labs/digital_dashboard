add_library(core STATIC
    core.cpp
)

target_include_directories(core PUBLIC
    include
)

# The checkout the binary was built from: paths::resource() falls back to it
# when a shipped file is not found next to the binary, so developer builds find
# eds/ and friends in the tree. On the target the install layout wins first.
target_compile_definitions(core PRIVATE
    REDLINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# cxxopts is deliberately gone: this used to parse the command line as well as
# set up logging, under a hardcoded program name. Argument parsing lives in
# libs/cli (multi-verb tools) or in each node's own main().
target_link_libraries(core PRIVATE
    spdlog::spdlog
)

add_executable(core_test
    tests/test_core.cpp
)
target_link_libraries(core_test PRIVATE
    core
    spdlog::spdlog
)
add_test(NAME core_test COMMAND core_test)
