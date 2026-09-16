cmake_minimum_required(VERSION 3.10)

project(racegrade_tc8)

# The decoded E888 messages onto their schemas. A library so the mapping can be
# tested without a bus; see racegrade_tc8_messages.h.
add_library(racegrade_tc8_messages STATIC
    racegrade_tc8_messages.cpp
)

target_include_directories(racegrade_tc8_messages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(racegrade_tc8_messages PUBLIC
    capnp
    schemas
    dbc_motec_e888_rev1
)

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
    node_health
    cli
    dbc_motec_e888_rev1
    racegrade_tc8_messages
    core
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
)

# Eight voltages, eight thermocouples and four frequencies in one multiplexed
# message, in order and indistinguishable by value: the mapping is checked with
# a different number in every source field.
add_executable(racegrade_tc8_test_messages
    test_racegrade_tc8_messages.cpp
)

target_link_libraries(racegrade_tc8_test_messages PRIVATE
    racegrade_tc8_messages
)

add_project_test(TARGET racegrade_tc8_test_messages LABELS racegrade_tc8 unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET racegrade_tc8 COMPONENT nodes)
redline_install(TARGET test_racegrade_tc8_query COMPONENT nodes)
