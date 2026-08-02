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

add_executable(pcan_trc_parser_test_parse
    test/test_trc_parse.cpp
)

target_link_libraries(pcan_trc_parser_test_parse
    PRIVATE
        pcan::trc_parser
        spdlog::spdlog
        helpers
)
add_project_test(TARGET pcan_trc_parser_test_parse LABELS pcan_trc_parser unit)


