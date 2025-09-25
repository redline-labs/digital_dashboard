cmake_minimum_required(VERSION 3.10)

project(msel_master_relay)

add_executable(msel_master_relay
    main.cpp
)

target_include_directories(msel_master_relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(msel_master_relay PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    dbc_msel_master_relay
)
