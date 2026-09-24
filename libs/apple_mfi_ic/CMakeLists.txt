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
    i2c_bus
    OpenSSL::SSL
    OpenSSL::Crypto

    PRIVATE
        # Link common logging interface (wraps spdlog) - defined in parent CMakeLists.txt
        spdlog::spdlog
)

# Demo executable to test out the library.
add_executable(apple_mfi_demo apple_mfi_demo.cpp)

target_link_libraries(apple_mfi_demo PRIVATE
    apple_mfi_ic
    # Link common logging interface (wraps spdlog) - defined in parent CMakeLists.txt
    spdlog::spdlog
)

# The retry policy against a fake coprocessor with the measured timing, on a
# native-controller and a bridge-shaped transport, each on a loaded host whose
# sleeps come back late, and a host frozen for longer than a whole operation's
# budget. Real time throughout; the default timeout catches a hang, since the test
# itself asserts no durations.
add_executable(apple_mfi_ic_test tests/test_apple_mfi_ic.cpp)
target_link_libraries(apple_mfi_ic_test PRIVATE
    apple_mfi_ic
    spdlog::spdlog
)
add_project_test(TARGET apple_mfi_ic_test LABELS apple_mfi_ic unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET apple_mfi_demo COMPONENT nodes)
