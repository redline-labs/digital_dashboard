cmake_minimum_required(VERSION 3.10)

project(apple_usb)

# Linux-only: this library talks to iPhones through raw usbfs ioctls
# (/dev/bus/usb), sysfs, and TUN/TAP. The parent CMakeLists gates it.
set(APPLE_USB_SOURCES
    usb_device.cpp
    muxd.cpp
    ncm_bridge.cpp
)

# The usbmux socket bridge + lockdown client depend on libplist and
# libimobiledevice (LGPL-2.1+, GPL-3 compatible), consumed as system packages
# via pkg-config. When they are absent the library still builds (USB detect +
# raw mux), but the lockdown path is compiled out and openCarkitChannel()
# returns nullptr -- mirroring how apple_mfi_ic degrades without OpenSSL.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPLIST IMPORTED_TARGET libplist-2.0)
    pkg_check_modules(LIBIMOBILEDEVICE IMPORTED_TARGET libimobiledevice-1.0)
endif()

add_library(apple_usb STATIC ${APPLE_USB_SOURCES})

target_include_directories(apple_usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(apple_usb PRIVATE
    spdlog::spdlog
)

if(LIBPLIST_FOUND AND LIBIMOBILEDEVICE_FOUND)
    target_sources(apple_usb PRIVATE
        usbmuxd_server.cpp
        lockdown.cpp
    )
    target_link_libraries(apple_usb PUBLIC
        PkgConfig::LIBPLIST
        PkgConfig::LIBIMOBILEDEVICE
    )
    target_compile_definitions(apple_usb
        PRIVATE APPLE_USB_HAVE_LIBIMOBILEDEVICE=1
        PUBLIC  APPLE_USB_HAVE_LOCKDOWN=1
    )
    message(STATUS "apple_usb: libimobiledevice found; wired CarPlay lockdown enabled")
else()
    target_sources(apple_usb PRIVATE lockdown.cpp)  # provides the stub fallback
    message(WARNING "apple_usb: libimobiledevice/libplist not found; wired CarPlay lockdown disabled")
endif()
