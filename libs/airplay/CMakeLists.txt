cmake_minimum_required(VERSION 3.10)

project(airplay)

find_package(OpenSSL QUIET)

if (NOT TARGET OpenSSL::Crypto)
    message(WARNING "OpenSSL not found, skipping airplay library (not required for dashboard)")
    return()
endif()

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
)

target_include_directories(airplay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(airplay
    PUBLIC
        OpenSSL::Crypto

    PRIVATE
        spdlog::spdlog
)

# Round-trip and known-answer tests, runnable without hardware.
foreach(test tlv8 crypto plist nalu)
    add_executable(airplay_test_${test} test_${test}.cpp)
    target_link_libraries(airplay_test_${test} PRIVATE
        airplay
        spdlog::spdlog
    )
endforeach()
