cmake_minimum_required(VERSION 3.10)

project(airplay)

find_package(OpenSSL QUIET)

if (NOT TARGET OpenSSL::Crypto)
    message(WARNING "OpenSSL not found, skipping airplay library (not required for dashboard)")
    return()
endif()

# libavcodec decodes the phone's AAC-LC entertainment audio stream (type 102)
# to PCM. It is already a dependency of the video path here; the AAC decoder is
# built into the same library.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AIRPLAY_LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)

# CarPlay AirPlay/RTSP session stack (transport-agnostic TCP server side).
add_library(airplay STATIC
    tlv8.cpp
    crypto.cpp
    channel_crypto.cpp
    srp.cpp
    pairing_session.cpp
    nalu.cpp
    media_stream.cpp
    mic_uplink.cpp
    rtsp.cpp
    event_queue.cpp
    event_channel.cpp
    net.cpp
    hid.cpp
    oem_button.cpp
    info_plist.cpp
    receiver.cpp
    timing.cpp
    aac_decoder.cpp
)

target_include_directories(airplay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(airplay
    PUBLIC
        OpenSSL::Crypto
        helpers
        plist

    PRIVATE
        spdlog::spdlog
        PkgConfig::AIRPLAY_LIBAV
)

# Round-trip and known-answer tests, runnable without hardware.
foreach(test tlv8 crypto nalu event_queue oem_button hid channel_crypto info_plist pairing_session rtsp mic_uplink timing)
    add_executable(airplay_test_${test} test_${test}.cpp)
    target_link_libraries(airplay_test_${test} PRIVATE
        airplay
        spdlog::spdlog
    )
    add_project_test(TARGET airplay_test_${test} LABELS airplay unit)
endforeach()

# The AAC round-trip needs the codec library directly (encoder side).
add_executable(airplay_test_aac test_aac.cpp)
target_link_libraries(airplay_test_aac PRIVATE
    airplay
    spdlog::spdlog
    PkgConfig::AIRPLAY_LIBAV
)
add_project_test(TARGET airplay_test_aac LABELS airplay unit)
