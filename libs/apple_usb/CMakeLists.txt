cmake_minimum_required(VERSION 3.10)

project(apple_usb)

# Every source here builds on every platform, which is what lets the whole
# library be built and tested on a developer's macOS box:
#
#   * The protocol and service layer -- the usbmux state machine, the usbmuxd
#     socket server, the lockdown client -- is plain C++ and POSIX sockets.
#   * The USB transport is libusb, which is portable. Enumeration, descriptor
#     parsing, configuration selection and every transfer build everywhere.
#
# There used to be a platform split here for the NCM bridge, which drove NTB16
# framing over usbfs into a TAP device and needed /dev/net/tun, AF_INET6 ioctls
# and /proc/sys/net/ipv6. It is gone: the system NCM driver (AppleUSBNCM on
# macOS, cdc_ncm on Linux) already builds that link when the CarPlay
# configuration is applied, so the node looks the interface up instead of
# rebuilding it. All that survives here is descriptor discovery
# (ncm_discovery.cpp), which is how the right interface is identified.
#
# usb_device.h stays a clean interface (libusb_device_handle is forward
# declared, <libusb.h> is not included) so nothing above it has to know what
# backs it.
set(APPLE_USB_SOURCES
    usb_device.cpp
    ncm_discovery.cpp
    muxd.cpp
    usbmuxd_server.cpp
    usbmuxd_framing.cpp
    usbmux_client.cpp
    pair_record.cpp
    tls_stream.cpp
    lockdown_client.cpp
    carkit_channel.cpp
)

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

# usbmuxd control framing: the header bounds, and the UDID spelling conversion.
add_executable(apple_usb_test_usbmuxd_framing test_usbmuxd_framing.cpp)
target_link_libraries(apple_usb_test_usbmuxd_framing PRIVATE
    apple_usb
    plist
    spdlog::spdlog
)
add_project_test(TARGET apple_usb_test_usbmuxd_framing LABELS apple_usb unit)
