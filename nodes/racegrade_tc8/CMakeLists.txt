cmake_minimum_required(VERSION 3.10)

project(racegrade_tc8)

add_executable(racegrade_tc8
    main.cpp
    tc8_can_frame_parser.cpp
)

target_include_directories(racegrade_tc8 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(racegrade_tc8 PRIVATE
    # Logging and CLI
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    motec_e888_rev1
)

add_executable(test_racegrade_tc8_query
    test/test_racegrade_tc8_query.cpp
)

target_link_libraries(test_racegrade_tc8_query PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    motec_e888_rev1
)
