add_subdirectory(helpers)
add_subdirectory(apple_mfi_ic) 
add_subdirectory(pub_sub)
add_subdirectory(mcp2221a)
add_subdirectory(i2c_bus)
add_subdirectory(reflection)
add_subdirectory(agent_control)
add_subdirectory(dbc_parser)
add_subdirectory(core)
add_subdirectory(pcan_trc_parser)
add_subdirectory(canopen)

# Wired CarPlay stack (ported from LIVI, GPL-3.0-or-later).
# plist comes first: both airplay (binary plists on the RTSP channel) and
# apple_usb (XML plists on the usbmux and lockdown sockets) build on it.
add_subdirectory(plist)
add_subdirectory(iap2)
add_subdirectory(airplay)
# Built everywhere. The usbfs/sysfs/TUN transport inside it is Linux-only and
# gets stubbed elsewhere, but the mux/usbmuxd/lockdown protocol layer is
# portable -- which is what makes it testable without a Linux box or a phone.
add_subdirectory(apple_usb)
