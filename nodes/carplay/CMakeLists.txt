cmake_minimum_required(VERSION 3.10)

project(carplay)

# The whole node builds on every platform. apple_usb carries the platform split
# internally (a null NcmBridge off Linux), so nothing here is conditional: the
# bring-up pipeline is compiled and linked on a developer's macOS box too, which
# is the only way a refactor there gets caught before it reaches hardware.
add_executable(carplay
    main.cpp
    zenoh_bridge.cpp
    simulate.cpp
    usb_pipeline.cpp
    iap2_session.cpp
)

# libavcodec is used by --simulate to generate a synthetic H.264 stream (and
# later for AAC/Opus decode of the phone's audio).
find_package(PkgConfig REQUIRED)
pkg_check_modules(CARPLAY_NODE_LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)

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
    apple_usb
    PkgConfig::CARPLAY_NODE_LIBAV
)
