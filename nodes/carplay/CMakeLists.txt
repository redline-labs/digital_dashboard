cmake_minimum_required(VERSION 3.10)

project(carplay)

add_executable(carplay
    main.cpp
    zenoh_bridge.cpp
    simulate.cpp
)

# libavcodec is used by --simulate to generate a synthetic H.264 stream (and
# later for AAC/Opus decode of the phone's audio).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)

target_include_directories(carplay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(carplay PRIVATE
    spdlog::spdlog
    cxxopts::cxxopts
    zenohcxx::zenohc
    schemas
    zenoh_pub_sub
    iap2
    airplay
    PkgConfig::LIBAV
)

# The USB transport (usbfs/TUN) only exists on Linux; everything above it
# stays portable so the node skeleton builds on macOS for development.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(carplay PRIVATE apple_usb)
    target_compile_definitions(carplay PRIVATE CARPLAY_HAVE_APPLE_USB=1)
endif()
