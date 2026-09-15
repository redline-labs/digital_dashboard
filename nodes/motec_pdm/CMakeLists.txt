cmake_minimum_required(VERSION 3.10)

project(motec_pdm)

# The decoded messages onto their schemas. A library so the list lengths can be
# tested against the schema's $fixedLength without a bus; see pdm_messages.h.
add_library(motec_pdm_messages STATIC
    pdm_messages.cpp
)

target_include_directories(motec_pdm_messages PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(motec_pdm_messages PUBLIC
    capnp
    schemas
    dbc_motec_pdm_generic_output
)

add_executable(motec_pdm
    main.cpp
)

target_link_libraries(motec_pdm PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
    motec_pdm_messages
)

add_executable(motec_pdm_test_messages
    test_pdm_messages.cpp
)

target_link_libraries(motec_pdm_test_messages PRIVATE
    motec_pdm_messages
    zenoh_pub_sub
    spdlog::spdlog
)

add_project_test(TARGET motec_pdm_test_messages LABELS motec_pdm unit)
