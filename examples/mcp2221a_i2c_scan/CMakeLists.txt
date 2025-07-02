add_executable(mcp2221a_i2c_scan main.cpp)

target_link_libraries(mcp2221a_i2c_scan PRIVATE
    mcp2221a
    spdlog::spdlog
) 