add_subdirectory(racegrade_tc8)
add_subdirectory(inspect)
# Record and replay the bus. A thin CLI over libs/bag.
add_subdirectory(bag)
add_subdirectory(msel_master_relay)
add_subdirectory(megasquirt)
add_subdirectory(motec_ltc)
add_subdirectory(motec_m1)
add_subdirectory(can_bridge)
add_subdirectory(motec_pdm)
add_subdirectory(grayhill_keypad)
add_subdirectory(carplay)
# Trimble BD992 GNSS receiver over Ethernet.
add_subdirectory(bd992_bridge)
# The same GNSS topics, with no receiver behind them: asks map_server for a
# route or a track centreline and drives it. The first caller of map/route and
# map/track_detail anywhere in the tree.
add_subdirectory(bd992_mock)
# The Xsens MTi-610 IMU. Same split as the BD992: libs/xbus parses, libs/mti610
# owns the serial port, and this maps what comes out onto schemas.
add_subdirectory(mti610_bridge)
# A Motorola MOTOTRBO radio over its USB-RNDIS link: what the radio is doing,
# and the channel as a service. Read-only except for the channel, which is
# gated by config.
add_subdirectory(xpr_bridge)
# A display module's backlight and its light and temperature sensors, from what
# the rootfs bound at boot: status out, brightness in. libs/display_backlight
# reads the files.
add_subdirectory(backlight)
# Offline maps: .mbtiles archives served as tiles, catalogs and style assets.
# The only node with no hardware behind it -- what it bridges is a file.
add_subdirectory(map_server)
# Which road the vehicle is on, and what is ahead of it. Opens the road graph
# directly rather than asking map_server -- a matcher makes dozens of lookups
# per fix, and a read-only mmap is shareable between processes.
add_subdirectory(map_match)
# The board's web console: system info, node health, service calls and RAUC
# reflash over HTTP. Not a bus gateway -- the browser is a real zenoh client,
# so what this serves is the half that was never on the bus.
add_subdirectory(web_console)
