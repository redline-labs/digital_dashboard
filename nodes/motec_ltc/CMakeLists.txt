cmake_minimum_required(VERSION 3.10)

project(motec_ltc)

add_executable(motec_ltc
    main.cpp
)

target_include_directories(motec_ltc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(motec_ltc PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    dbc_motec_ltc_rev1
)


