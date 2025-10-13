cmake_minimum_required(VERSION 3.10)

project(can_replay)

add_executable(can_replay
    main.cpp
)

target_link_libraries(can_replay PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    pcan_trc_parser
)


