cmake_minimum_required(VERSION 3.10)

project(i2c_bus)

# Which I2C backend talks to the MFi coprocessor.
#   auto     - kernel i2c-dev on Linux, hidapi/MCP2221A elsewhere (default)
#   linux    - force the kernel /dev/i2c-N path
#   mcp2221a - force the userspace hidapi path (needs hid_mcp2221 blacklisted
#              on Linux, since the kernel driver would otherwise own the chip)
set(I2C_BUS_BACKEND "auto" CACHE STRING "I2C backend: auto, linux, or mcp2221a")
set_property(CACHE I2C_BUS_BACKEND PROPERTY STRINGS auto linux mcp2221a)

if(I2C_BUS_BACKEND STREQUAL "auto")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_i2c_backend "linux")
    else()
        set(_i2c_backend "mcp2221a")
    endif()
else()
    set(_i2c_backend "${I2C_BUS_BACKEND}")
endif()

add_library(i2c_bus i2c_bus.cpp)

target_include_directories(i2c_bus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(i2c_bus PRIVATE spdlog::spdlog)

if(_i2c_backend STREQUAL "linux")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "I2C_BUS_BACKEND=linux requires Linux")
    endif()
    target_sources(i2c_bus PRIVATE linux_i2c_dev.cpp)
    target_compile_definitions(i2c_bus PRIVATE I2C_BUS_HAVE_LINUX_I2CDEV=1)
    message(STATUS "i2c_bus: kernel /dev/i2c-N backend")
elseif(_i2c_backend STREQUAL "mcp2221a")
    target_sources(i2c_bus PRIVATE mcp2221a_bus.cpp)
    target_link_libraries(i2c_bus PRIVATE mcp2221a)
    target_compile_definitions(i2c_bus PRIVATE I2C_BUS_HAVE_MCP2221A=1)
    message(STATUS "i2c_bus: MCP2221A hidapi backend")
else()
    message(FATAL_ERROR "unknown I2C_BUS_BACKEND '${I2C_BUS_BACKEND}'")
endif()
