cmake_minimum_required(VERSION 3.10)

project(iap2)

# iAP2 accessory protocol (transport-agnostic) + MFi auth signer.
# Requires the apple_mfi_ic library (and therefore OpenSSL).
add_library(iap2 STATIC
    link_layer.cpp
    mcp2221a_mfi_signer.cpp
    csm.cpp
    messages.cpp
    location_nmea.cpp
)

target_include_directories(iap2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(iap2
    PUBLIC
        apple_mfi_ic
    PRIVATE
        spdlog::spdlog
)

# Link framing and control session message tests, runnable without hardware.
add_executable(iap2_test_framing
    test_framing.cpp
)

target_link_libraries(iap2_test_framing PRIVATE
    iap2
    spdlog::spdlog
)
add_project_test(TARGET iap2_test_framing LABELS iap2 unit)

# NMEA generation for the GPS location uplink, runnable without hardware.
add_executable(iap2_test_nmea
    test_nmea.cpp
)

target_link_libraries(iap2_test_nmea PRIVATE
    iap2
    spdlog::spdlog
)
add_project_test(TARGET iap2_test_nmea LABELS iap2 unit)

# The control-session-message parameter codec, at its edges.
add_executable(iap2_test_csm test_csm.cpp)
target_link_libraries(iap2_test_csm PRIVATE
    iap2
    spdlog::spdlog
)
add_project_test(TARGET iap2_test_csm LABELS iap2 unit)
