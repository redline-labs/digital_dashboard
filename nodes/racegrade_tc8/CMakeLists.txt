cmake_minimum_required(VERSION 3.10)

project(racegrade_tc8)

add_executable(racegrade_tc8
    main.cpp
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
)

add_executable(racegrade_tc8_querier
    test/main.cpp
)

target_link_libraries(racegrade_tc8_querier PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
)