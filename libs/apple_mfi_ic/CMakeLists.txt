find_package(OpenSSL QUIET)

if (NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
    message(WARNING "OpenSSL not found, skipping apple_mfi_ic library (not required for dashboard)")
    return()
endif()

add_library(apple_mfi_ic apple_mfi_ic.cpp)

target_include_directories(apple_mfi_ic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(apple_mfi_ic PUBLIC
    mcp2221a
    OpenSSL::SSL
    OpenSSL::Crypto

    PRIVATE
        # Link common logging interface (wraps spdlog) - defined in parent CMakeLists.txt
        common_logging
)

# Demo executable to test out the library.
add_executable(apple_mfi_demo apple_mfi_demo.cpp)

target_link_libraries(apple_mfi_demo PRIVATE
    apple_mfi_ic
    # Link common logging interface (wraps spdlog) - defined in parent CMakeLists.txt
    common_logging
)
