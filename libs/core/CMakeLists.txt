add_library(core STATIC
    core.cpp
)

target_include_directories(core PUBLIC
    include
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
add_project_test(TARGET core_test LABELS core unit)
