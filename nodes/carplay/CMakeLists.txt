cmake_minimum_required(VERSION 3.10)

project(carplay)

# Config parsing is its own library so a test can link it without dragging in
# the node's USB, zenoh and libav dependencies. It is the only part of the node
# that is pure enough to test directly.
add_library(carplay_node_config STATIC node_config.cpp)
target_include_directories(carplay_node_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(carplay_node_config
    PUBLIC
        airplay
        iap2
    PRIVATE
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

# The whole node builds on every platform, and nothing here is conditional: the
# bring-up pipeline is compiled and linked on a developer's macOS box too, which
# is the only way a refactor there gets caught before it reaches hardware.
add_executable(carplay
    main.cpp
    zenoh_bridge.cpp
    simulate.cpp
    usb_pipeline.cpp
    screen_handover.cpp
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
    core
    node_health
    carplay_node_config
    helpers_ffmpeg
    spdlog::spdlog
    cxxopts::cxxopts
    yaml-cpp::yaml-cpp
    zenohcxx::zenohc
    schemas
    zenoh_pub_sub
    iap2
    airplay
    apple_usb
    PkgConfig::CARPLAY_NODE_LIBAV
)

# Config parsing: the YAML surface and the PNG header reader behind it.
add_executable(carplay_test_node_config test_node_config.cpp)
target_link_libraries(carplay_test_node_config PRIVATE
    carplay_node_config
    helpers_ffmpeg
    spdlog::spdlog
)
# The committed artwork is part of what is under test -- a config that ships
# with the repo should load.
target_compile_definitions(carplay_test_node_config PRIVATE
    CARPLAY_CONFIG_DIR="${CMAKE_SOURCE_DIR}/configs/carplay"
)
add_project_test(TARGET carplay_test_node_config LABELS carplay unit)

# When to hand the screen between the phone and the car. Pure state machine: no
# phone, no bus.
add_executable(carplay_test_screen_handover test_screen_handover.cpp screen_handover.cpp)
target_link_libraries(carplay_test_screen_handover PRIVATE
    airplay
    spdlog::spdlog
)
add_project_test(TARGET carplay_test_screen_handover LABELS carplay unit)

# When CarPlayStartSession goes out, given an NCM address that may turn up late.
add_executable(carplay_test_start_session_gate test_start_session_gate.cpp)
add_project_test(TARGET carplay_test_start_session_gate LABELS carplay unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET carplay COMPONENT nodes)
