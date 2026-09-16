# Add comprehensive test data publisher executable
add_executable(test_data_publisher
    test_data_publisher.cpp
)

target_link_libraries(test_data_publisher PRIVATE
    # Link common logging interface (wraps spdlog)
    spdlog::spdlog
    
    # Link our schemas library
    schemas
    helpers
    zenoh_pub_sub
)

add_executable(mock_racegrade_tc8_frames
    mock_racegrade_tc8_frames.cpp
)

target_link_libraries(mock_racegrade_tc8_frames PRIVATE
    spdlog::spdlog
    schemas
    zenoh_pub_sub
    cxxopts::cxxopts
    dbc_motec_e888_rev1
)

# SHIPPED as bring-up tooling, in its own component so a production image can drop it
# without a recipe change: nothing that invents vehicle data belongs on a shipping cluster.
redline_install(TARGET test_data_publisher COMPONENT mockdata)
redline_install(TARGET mock_racegrade_tc8_frames COMPONENT mockdata)
