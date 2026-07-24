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
    srp.cpp
    plist.cpp
    nalu.cpp
    rtsp.cpp
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

    PRIVATE
        spdlog::spdlog
        PkgConfig::AIRPLAY_LIBAV
)

# Round-trip and known-answer tests, runnable without hardware.
foreach(test tlv8 crypto plist nalu)
    add_executable(airplay_test_${test} test_${test}.cpp)
    target_link_libraries(airplay_test_${test} PRIVATE
        airplay
        spdlog::spdlog
    )
endforeach()

# The AAC round-trip needs the codec library directly (encoder side).
add_executable(airplay_test_aac test_aac.cpp)
target_link_libraries(airplay_test_aac PRIVATE
    airplay
    spdlog::spdlog
    PkgConfig::AIRPLAY_LIBAV
)
