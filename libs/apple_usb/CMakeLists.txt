cmake_minimum_required(VERSION 3.10)

project(apple_usb)

# This library splits along a hardware line, and the split is what lets most of
# it build (and be tested) on a developer's macOS box:
#
#   * The protocol and service layer -- the usbmux state machine, the usbmuxd
#     socket server, the lockdown client -- is plain C++ and POSIX sockets.
#     Portable, and where the logic worth unit testing lives.
#   * The transport -- raw usbfs ioctls against /dev/bus/usb, and the TUN/TAP
#     NCM bridge -- is Linux-only by construction.
#
# usb_device.h is deliberately a clean interface (no linux/ headers, USBDEVFS
# named only in comments), so the portable half compiles against it anywhere.
# On non-Linux the implementation is a stub that fails every call, which is
# enough to link tests that drive the protocol without touching a phone.
set(APPLE_USB_SOURCES
    muxd.cpp
    usbmuxd_server.cpp
    usbmux_client.cpp
    pair_record.cpp
    tls_stream.cpp
    lockdown_client.cpp
    carkit_channel.cpp
    lockdown.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND APPLE_USB_SOURCES
        usb_device.cpp
        ncm_bridge.cpp
    )
else()
    list(APPEND APPLE_USB_SOURCES usb_device_stub.cpp)
endif()

add_library(apple_usb STATIC ${APPLE_USB_SOURCES})

target_include_directories(apple_usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(apple_usb PRIVATE
    spdlog::spdlog
)

# Our own property lists. The usbmuxd socket server, the usbmux client and the
# lockdown client are off libplist entirely; what remains of the vendored stack
# below still uses its own copy internally.
target_link_libraries(apple_usb PUBLIC plist)

# The native lockdown backend runs its own TLS: two client-certificate sessions
# (the lockdown session, and the carkit service connection) out of the pair
# record's root key pair.
find_package(OpenSSL REQUIRED)
target_link_libraries(apple_usb PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# libimobiledevice and libplist come from third_party/libimobiledevice.cmake,
# built from pinned sources rather than whatever the host distro ships -- we
# depend on version-specific behaviour (see the UDID normalisation note in
# lockdown.cpp). The old pkg-config path and its compiled-out fallback are gone;
# the dependency is always present now, so lockdown is always built.
target_link_libraries(apple_usb PUBLIC limd)
target_compile_definitions(apple_usb
    PRIVATE APPLE_USB_HAVE_LIBIMOBILEDEVICE=1
    PUBLIC  APPLE_USB_HAVE_LOCKDOWN=1
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Flag it once at configure time so nobody mistakes a green build here for
    # a working transport.
    target_compile_definitions(apple_usb PUBLIC APPLE_USB_NO_TRANSPORT=1)
    message(STATUS
        "apple_usb: non-Linux host -- protocol layer built, USB/NCM transport stubbed")
endif()

# Protocol-level tests. These drive the portable half against mocks, so they run
# on any host -- no phone, no usbfs.
add_executable(apple_usb_test_usbmux_client test_usbmux_client.cpp)
target_link_libraries(apple_usb_test_usbmux_client PRIVATE
    apple_usb
    spdlog::spdlog
)

# Hardware counterpart to the test above: points our client at a real running
# UsbmuxdServer. Replaces the `idevice_id -l` check in the bring-up doc, which
# proved the server using someone else's client.
add_executable(apple_usb_muxctl muxctl.cpp)
target_link_libraries(apple_usb_muxctl PRIVATE
    apple_usb
    spdlog::spdlog
)
