add_library(mcp2221a mcp2221a.cpp)

target_include_directories(mcp2221a PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp2221a PUBLIC
    hidapi

    PRIVATE
        # Link common logging interface (wraps spdlog) - defined in parent CMakeLists.txt
        common_logging
) 

# Demo executable to test out the library.
add_executable(mcp2221a_i2c_scan mcp2221a_i2c_scan.cpp)

target_link_libraries(mcp2221a_i2c_scan PRIVATE
    mcp2221a

    # Link common logging interface (wraps spdlog) - defined in parent CMakeLists.txt
    common_logging
) 