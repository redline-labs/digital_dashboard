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
# The MoTeC UTC, over libusb. After `can` for the same reason can_pcan is.
add_subdirectory(can_motec)
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
# no zenoh and no Qt -- nodes/map_server is what puts it on the bus. The
# dashboard must not link this, and scope deliberately does; see the note in
# its CMakeLists.
add_subdirectory(mbtiles)
# The race-track catalogue, which lives in extra tables INSIDE an .mbtiles.
# After mbtiles, whose Archive must stay unable to see it and whose Writer
# must have closed the file before this one opens it.
add_subdirectory(track_store)
# Just enough protobuf for the two formats in this tree that are protobuf.
# Before mvt and osm, both of which link it.
add_subdirectory(protowire)
# Mapbox Vector Tiles, decoded. After mbtiles only because its real-data test
# reads an archive through it; nothing in the library itself depends on it.
add_subdirectory(mvt)
# OpenStreetMap PBF, decoded. The source everything on the map side is built
# from; see docs/map_build.md.
add_subdirectory(osm)
# One set of rules for what OSM data is. Both the tiler and the graph builder
# call it, which is the whole reason the extractor is ours.
add_subdirectory(map_rules)
# The routable road network on disk, mmap'd. After map_rules, which its
# segment records carry.
add_subdirectory(road_graph)
# A segment on the wire. After road_graph and map_rules, whose types it maps,
# and after schemas, whose vocabulary it maps them to.
add_subdirectory(map_wire)
# Vector tiles to pixels: projection, tessellator, GPU renderer, label layout,
# tile cache. Hoisted out of dashboard/widgets/map so scope can draw a map
# without reaching into the dashboard's include tree. After mvt, which it
# decodes tiles with; it knows nothing about where the bytes came from.
add_subdirectory(map_render)
# The floating buttons both map surfaces overlay on their maps. QWidgets,
# which is exactly what map_render must not hold -- hence a sibling, after it
# only for reading order; it links only Qt.
add_subdirectory(map_controls)
# The MOTOTRBO radio protocols: XNL framing and authentication, XCMP commands,
# the on-air control plane, and the NAI data service codecs. Pure bytes, no
# I/O -- `xpr` sits above it with the sockets, the same way bd992 sits above
# gsof. Nothing here may depend on that.
add_subdirectory(mototrbo)
# The radio itself: TCP, the XNL session state machine, and the typed queries
# a node needs. After `mototrbo`, which it links.
add_subdirectory(xpr)
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
