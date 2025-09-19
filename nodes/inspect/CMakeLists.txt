cmake_minimum_required(VERSION 3.10)

project(inspect)

add_executable(inspect
    main.cpp
    dump.cpp
)

target_link_libraries(inspect PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    zenoh_pub_sub
)


