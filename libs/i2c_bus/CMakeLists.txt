cmake_minimum_required(VERSION 3.10)

project(i2c_bus)

add_library(i2c_bus i2c_bus.cpp)

target_include_directories(i2c_bus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(i2c_bus PRIVATE spdlog::spdlog)

# The backend follows the host, because that is the only thing it can follow:
# Linux has an in-kernel driver for the MCP2221A (hid_mcp2221) that registers it
# as a normal I2C adapter, and macOS does not, so there it has to be driven over
# USB HID from userspace. On a deployed Linux board the coprocessor hangs off the
# SoC's own I2C controller and the same kernel path serves it unchanged.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(i2c_bus PRIVATE linux_i2c_dev.cpp)
    target_compile_definitions(i2c_bus PRIVATE I2C_BUS_HAVE_LINUX_I2CDEV=1)
    message(STATUS "i2c_bus: kernel /dev/i2c-N backend")
else()
    target_sources(i2c_bus PRIVATE mcp2221a_bus.cpp)
    target_link_libraries(i2c_bus PRIVATE mcp2221a)
    target_compile_definitions(i2c_bus PRIVATE I2C_BUS_HAVE_MCP2221A=1)
    message(STATUS "i2c_bus: MCP2221A hidapi backend")
endif()
