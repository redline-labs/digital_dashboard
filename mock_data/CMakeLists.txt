# Add comprehensive test data publisher executable
add_executable(test_data_publisher
    test_data_publisher.cpp
)

target_link_libraries(test_data_publisher PRIVATE
    # Link common logging interface (wraps spdlog)
    common_logging
    
    # Link our schemas library
    schemas
    helpers
    zenoh_pub_sub
)

add_executable(mock_racegrade_tc8_frames
    mock_racegrade_tc8_frames.cpp
)

target_link_libraries(mock_racegrade_tc8_frames PRIVATE
    common_logging
    schemas
    zenoh_pub_sub
    cxxopts::cxxopts
    dbc_motec_e888_rev1
)
