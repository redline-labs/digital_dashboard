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
