cmake_minimum_required(VERSION 3.10)

project(motec_pdm)

add_executable(motec_pdm
    main.cpp
)

target_link_libraries(motec_pdm PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    dbc_motec_pdm_generic_output
)


