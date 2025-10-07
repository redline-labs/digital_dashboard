cmake_minimum_required(VERSION 3.10)

project(motec_m1)

add_executable(motec_m1
    main.cpp
)

target_include_directories(motec_m1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(motec_m1 PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    dbc_motec_m1_rev3
)


