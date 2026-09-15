cmake_minimum_required(VERSION 3.10)

project(motec_m1)

# The decoded M1 messages onto their schemas -- about two hundred fields. A
# library so the wiring between the tested decoder and the tested schema can
# itself be tested; see motec_m1_messages.h.
add_library(motec_m1_messages STATIC
    motec_m1_messages.cpp
)

target_include_directories(motec_m1_messages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(motec_m1_messages PUBLIC
    capnp
    schemas
    dbc_motec_m1_rev3
)

add_executable(motec_m1
    main.cpp
)

target_include_directories(motec_m1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(motec_m1 PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    node_health
    cli
    dbc_motec_m1_rev3
    motec_m1_messages
)



# The mappings a differential test cannot see: which signal becomes which field,
# and the four messages assembled from two frames each.
add_executable(motec_m1_test_messages
    test_motec_m1_messages.cpp
)

target_link_libraries(motec_m1_test_messages PRIVATE
    motec_m1_messages
)

add_project_test(TARGET motec_m1_test_messages LABELS motec_m1 unit)
