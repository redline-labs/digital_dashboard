add_subdirectory(helpers)
add_subdirectory(apple_mfi_ic) 
add_subdirectory(pub_sub)
add_subdirectory(mcp2221a)
add_subdirectory(reflection)
add_subdirectory(dbc_parser)
add_subdirectory(core)
add_subdirectory(pcan_trc_parser)
add_subdirectory(canopen)

# Wired CarPlay stack (ported from LIVI, GPL-3.0-or-later).
add_subdirectory(iap2)
add_subdirectory(airplay)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Raw usbfs/sysfs/TUN transport to the phone; Linux only.
    add_subdirectory(apple_usb)
endif()
