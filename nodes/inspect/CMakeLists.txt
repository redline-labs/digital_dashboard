cmake_minimum_required(VERSION 3.10)

project(inspect)

add_executable(inspect
    main.cpp
    dump.cpp
    info.cpp
    list.cpp
    nodes.cpp
    hz.cpp
)

target_include_directories(inspect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(inspect PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    zenoh_pub_sub
)


