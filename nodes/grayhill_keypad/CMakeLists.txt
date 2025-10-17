cmake_minimum_required(VERSION 3.10)

project(grayhill_keypad)

add_executable(grayhill_keypad
    main.cpp
)

target_link_libraries(grayhill_keypad
    PRIVATE
        canopen_grayhill
        canopen

    	zenoh_pub_sub
        spdlog::spdlog
        cxxopts::cxxopts
        capnp
        schemas
        helpers
)

