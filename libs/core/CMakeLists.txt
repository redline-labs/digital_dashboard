add_library(core STATIC
    core.cpp
)

target_include_directories(core PUBLIC
    include
)

target_link_libraries(core PRIVATE
    cxxopts
    common_logging
)