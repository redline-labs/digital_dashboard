add_library(pcan_trc_parser
    src/pcan_trc_parser.cpp
)

target_include_directories(pcan_trc_parser
    PUBLIC
        include
)

target_link_libraries(pcan_trc_parser
    PRIVATE
        spdlog::spdlog
        lexy_core
        helpers
)

# Consumers can link with pcan_trc_parser via this alias
add_library(pcan::trc_parser ALIAS pcan_trc_parser)

add_executable(test_pcan_trc_parser
    test/test_parse.cpp
)

target_link_libraries(test_pcan_trc_parser
    PRIVATE
        pcan::trc_parser
        spdlog::spdlog
        helpers
)


