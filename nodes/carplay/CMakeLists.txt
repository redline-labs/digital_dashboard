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
# The zenoh side of the node, alone, so a test can drive it on a real session.
add_library(carplay_zenoh_bridge STATIC zenoh_bridge.cpp)
target_include_directories(carplay_zenoh_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(carplay_zenoh_bridge
    PUBLIC
        schemas
        zenoh_pub_sub
    PRIVATE
        spdlog::spdlog
)

add_executable(carplay
    main.cpp
    simulate.cpp
    usb_pipeline.cpp
    session_status.cpp
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
    carplay_zenoh_bridge
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

# What the dashboard is told about navigation and the track.
add_executable(carplay_test_metadata_merge test_metadata_merge.cpp)
target_link_libraries(carplay_test_metadata_merge PRIVATE carplay_zenoh_bridge iap2 spdlog::spdlog)
add_project_test(TARGET carplay_test_metadata_merge LABELS carplay unit)

# Which attached Apple device to bring up: a phone, and not the same failing
# one every time.
add_executable(carplay_test_device_picker test_device_picker.cpp)
target_link_libraries(carplay_test_device_picker PRIVATE apple_usb)
add_project_test(TARGET carplay_test_device_picker LABELS carplay unit)

# The GPS fix sent to the phone, and when it stops being one.
add_executable(carplay_test_location_cache test_location_cache.cpp)
add_project_test(TARGET carplay_test_location_cache LABELS carplay unit)

# Vehicle status: config values, live values over them key by key, and a change
# sent once while the phone is subscribed.
add_executable(carplay_test_vehicle_status_feed test_vehicle_status_feed.cpp)
add_project_test(TARGET carplay_test_vehicle_status_feed LABELS carplay unit)

# When the supervisor runs a phone's bring-up again: back off on failure, restart
# promptly when the phone ends a session that had been running.
add_executable(carplay_test_reattach_policy test_reattach_policy.cpp)
add_project_test(TARGET carplay_test_reattach_policy LABELS carplay unit)

# Detaching a bridge handler is a barrier: no call runs after set(nullptr).
add_executable(carplay_test_handler_slot test_handler_slot.cpp)
add_project_test(TARGET carplay_test_handler_slot LABELS carplay unit)

# What the dashboard is told as a phone comes and goes: phases, connected,
# the microphone, and publishing only on change. No bus.
add_executable(carplay_test_session_status test_session_status.cpp session_status.cpp)
target_link_libraries(carplay_test_session_status PRIVATE carplay_zenoh_bridge spdlog::spdlog)
add_project_test(TARGET carplay_test_session_status LABELS carplay unit)

# The bridge on a real session, with a renderer subscribed before it starts.
add_executable(carplay_test_zenoh_bridge test_zenoh_bridge.cpp)
target_link_libraries(carplay_test_zenoh_bridge PRIVATE carplay_zenoh_bridge spdlog::spdlog)
add_project_test(TARGET carplay_test_zenoh_bridge LABELS carplay net)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET carplay COMPONENT nodes)
