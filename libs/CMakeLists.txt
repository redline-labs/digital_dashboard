add_subdirectory(helpers)
add_subdirectory(apple_mfi_ic) 
add_subdirectory(pub_sub)
add_subdirectory(mcp2221a)
add_subdirectory(i2c_bus)
add_subdirectory(reflection)
# Both build on reflection, so they follow it.
add_subdirectory(config_codec)
add_subdirectory(qt_helpers)
add_subdirectory(agent_control)
add_subdirectory(dbc_parser)
add_subdirectory(core)
# Builds on core for the log pattern, and on pub_sub for --connect/--mode.
add_subdirectory(cli)
# Recording and replay (MCAP). After config_codec (reflected YAML) and schemas
# (the schema descriptors it embeds).
add_subdirectory(bag)
add_subdirectory(canopen)
add_subdirectory(can)
add_subdirectory(can_pcan)
add_subdirectory(can_socketcan)
# Reads and writes PCAN .trc traces, and turns one into a replay channel. After
# `can`: it uses can::dlc_to_length and implements can::Backend.
add_subdirectory(can_trc)
add_subdirectory(can_backends)
# The Trimble GSOF protocol: framing, records, command building. Pure bytes,
# no I/O -- `bd992` sits above it with the sockets. Independent of everything
# above, so its position here is only about keeping the hardware libraries
# together.
add_subdirectory(gsof)
# TCP, threads and the read-before-write configuration logic. After `gsof`,
# which it links; nothing in `gsof` may depend on this.
add_subdirectory(bd992)
# The mbtiles 1.3 format: an SQLite file full of map tiles. Pure file reading,
# no zenoh and no Qt -- nodes/map_server is what puts it on the bus. The GUI
# apps must not link this; see the note in its CMakeLists.
add_subdirectory(mbtiles)
# Mapbox Vector Tiles, decoded. After mbtiles only because its real-data test
# reads an archive through it; nothing in the library itself depends on it.
add_subdirectory(mvt)
# The MSEL Master Relay protocol. Links the generated dbc_msel_master_relay,
# which is only declared later in dbcs/ -- that subdirectory has to follow this
# one, because the generator it calls is defined in libs/dbc_parser. The forward
# reference is fine: CMake resolves target names when it generates, not when it
# reads.
add_subdirectory(msel)

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
