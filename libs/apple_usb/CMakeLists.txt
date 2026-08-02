cmake_minimum_required(VERSION 3.10)

project(apple_usb)

# This library splits along a hardware line, and the split is what lets most of
# it build (and be tested) on a developer's macOS box:
#
#   * The protocol and service layer -- the usbmux state machine, the usbmuxd
#     socket server, the lockdown client -- is plain C++ and POSIX sockets.
#   * The USB transport is libusb, which is portable. Enumeration, descriptor
#     parsing, configuration selection and every transfer build everywhere.
#   * Only the NCM bridge's TAP/IP plumbing is Linux-only, and not because of
#     USB: it needs /dev/net/tun, AF_INET6 ioctls and /proc/sys/net/ipv6.
#
# So the Linux-only boundary is the *network* plumbing, not USB. usb_device.h
# stays a clean interface (libusb_device_handle is forward declared, <libusb.h>
# is not included) so nothing above it has to know what backs it.
set(APPLE_USB_SOURCES
    usb_device.cpp
    ncm_discovery.cpp
    ncm_frame.cpp
    muxd.cpp
    usbmuxd_server.cpp
    usbmux_client.cpp
    pair_record.cpp
    tls_stream.cpp
    lockdown_client.cpp
    carkit_channel.cpp
)

# The bridge is the one platform-split file here, and it is a *substitution*,
# not an omission: the null backend defines the same four out-of-line NcmBridge
# methods, so NcmBridge is a complete type that links on every platform and no
# caller needs an #ifdef around it. Everything the bridge does that is not TAP
# -- descriptor discovery (ncm_discovery.cpp) and NTB16 framing (ncm_frame.cpp)
# -- is in the portable list above, compiled and unit tested everywhere.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND APPLE_USB_SOURCES ncm_bridge.cpp)
else()
    list(APPEND APPLE_USB_SOURCES ncm_bridge_null.cpp)
endif()

add_library(apple_usb STATIC ${APPLE_USB_SOURCES})

target_include_directories(apple_usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(apple_usb PRIVATE
    spdlog::spdlog
    usb-1.0
)

# Our own property lists, used by the usbmuxd socket server, the usbmux client,
# the lockdown client and the pair record store.
target_link_libraries(apple_usb PUBLIC plist)

# The native lockdown backend runs its own TLS: two client-certificate sessions
# (the lockdown session, and the carkit service connection) out of the pair
# record's root key pair.
find_package(OpenSSL REQUIRED)
target_link_libraries(apple_usb PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Defined so a consumer can *choose* to skip stage 6 rather than watch it
    # fail. It no longer gates compilation of anything: NcmBridge links here,
    # start() just refuses. Nothing in the tree tests it today, and that is the
    # point -- the code above the bridge is identical on both platforms.
    target_compile_definitions(apple_usb PUBLIC APPLE_USB_NO_NCM=1)
    message(STATUS
        "apple_usb: non-Linux host -- USB transport built, NCM bridge is the null backend")
endif()

# Protocol-level tests. These drive the portable half against mocks, so they run
# on any host -- no phone, no usbfs.
add_executable(apple_usb_test_usbmux_client test_usbmux_client.cpp)
target_link_libraries(apple_usb_test_usbmux_client PRIVATE
    apple_usb
    spdlog::spdlog
)
add_project_test(TARGET apple_usb_test_usbmux_client LABELS apple_usb unit)

# Descriptor-driven NCM discovery, against synthetic configuration descriptors.
# Runs anywhere: this is the coverage that stands in for a phone.
add_executable(apple_usb_test_ncm_discovery test_ncm_discovery.cpp)
target_link_libraries(apple_usb_test_ncm_discovery PRIVATE
    apple_usb
    spdlog::spdlog
)
add_project_test(TARGET apple_usb_test_ncm_discovery LABELS apple_usb unit)

# NTB16 framing, against synthetic transfer blocks. This is the AV data path --
# every video frame and audio packet the phone sends is parsed here -- and it
# had no coverage at all while it lived as private members of the Linux-only
# NcmBridge. Runs anywhere.
add_executable(apple_usb_test_ncm_frame test_ncm_frame.cpp)
target_link_libraries(apple_usb_test_ncm_frame PRIVATE
    apple_usb
    spdlog::spdlog
)
add_project_test(TARGET apple_usb_test_ncm_frame LABELS apple_usb unit)

# Pairing crypto: certificate shape, chain, and the on-disk round trip. Needs
# OpenSSL directly to inspect what the generator produced.
add_executable(apple_usb_test_pair_record test_pair_record.cpp)
target_link_libraries(apple_usb_test_pair_record PRIVATE
    apple_usb
    OpenSSL::Crypto
    spdlog::spdlog
)
add_project_test(TARGET apple_usb_test_pair_record LABELS apple_usb unit)

# Hardware counterpart to the test above: points our client at a real running
# UsbmuxdServer. Replaces the `idevice_id -l` check in the bring-up doc, which
# proved the server using someone else's client.
add_executable(apple_usb_muxctl muxctl.cpp)
target_link_libraries(apple_usb_muxctl PRIVATE
    apple_usb
    spdlog::spdlog
)

# Enumeration and descriptor dump, built on every platform. This is the first
# thing to run when hardware finally shows up: it exercises the whole libusb
# discovery path -- port paths, configuration descriptors, interface classes,
# endpoints, CDC functional descriptors -- without claiming anything or
# changing any state on the phone. See docs/carplay_bringup.md stage 1.
add_executable(apple_usb_usbprobe usbprobe.cpp)
target_link_libraries(apple_usb_usbprobe PRIVATE
    apple_usb
    spdlog::spdlog
)
