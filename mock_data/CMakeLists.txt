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
