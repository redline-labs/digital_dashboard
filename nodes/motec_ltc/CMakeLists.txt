cmake_minimum_required(VERSION 3.10)

project(motec_ltc)

# The decoded LTC message onto its schema. A library so the fault flags and the
# sensor-state mapping can be tested without a bus; see motec_ltc_messages.h.
add_library(motec_ltc_messages STATIC
    motec_ltc_messages.cpp
)

target_include_directories(motec_ltc_messages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(motec_ltc_messages PUBLIC
    capnp
    schemas
    dbc_motec_ltc_rev1
)

add_executable(motec_ltc
    main.cpp
)

target_include_directories(motec_ltc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(motec_ltc PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    node_health
    cli
    dbc_motec_ltc_rev1
    motec_ltc_messages
)



# Seven fault flags in a row and a sensor state mapped between two enumerations
# by hand: both compile whatever they say, and both are what a workshop reads.
add_executable(motec_ltc_test_messages
    test_motec_ltc_messages.cpp
)

target_link_libraries(motec_ltc_test_messages PRIVATE
    motec_ltc_messages
)

add_project_test(TARGET motec_ltc_test_messages LABELS motec_ltc unit)
