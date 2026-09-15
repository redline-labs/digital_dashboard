---
title: Libraries
nav_order: 5
---

# Libraries

Everything under `libs/` is reusable code with a CMake target of the same name
unless noted. These pages are for someone coding against a library: what it is,
what it deliberately is not, its public headers, and what will catch you out.
Every library is listed; the ones without a page yet say so.

## Bus, configuration and app plumbing

| Library | Target | What it is | Page |
|---|---|---|---|
| `pub_sub` | `zenoh_pub_sub` | Zenoh sessions, Cap'n Proto publish and subscribe, expression subscriptions, services, liveliness; the generated schema registry. | not written yet |
| `reflection` | `reflection` | `REFLECT_STRUCT` and `REFLECT_ENUM`: the compile-time field lists every codec and UI walks. | not written yet |
| `config_codec` | `config_codec` | Reflected-struct configuration: YAML and JSON both ways, self-description, validation by field path, clamping. No Qt. | not written yet |
| `core` | `core` | Paths, logging setup, systemd notify; the one place the runtime environment variables are read. | not written yet |
| `cli` | `cli` | Verb dispatch for the multi-verb tools: global options, exit codes, results on stdout. | not written yet |
| `helpers` | `helpers` | Qt-free odds and ends: hex, colour, unit conversion, a rate gate, the FFmpeg log bridge. | not written yet |
| `qt_helpers` | `qt_helpers` | The Qt-using sibling: a paint-caching widget base, colour conversion, resource fonts. | not written yet |
| `agent_control` | `agent_control` | The `--mcp` control socket embedded in every GUI app: methods, screenshots, input, the widget locator. | [Agent control](../developing/agent-control.html) |
| `dashboard_widgets` | `dashboard_widgets` | Every dashboard widget, the widget table, the layout config and its loader, the factory, and the `widget.*` agent methods; shared by the dashboard and the editor. | not written yet |
| `bag` | `bag` | Recording and replaying the bus as MCAP. | [bag](../nodes/bag.html) (node page, for now) |

## CAN

| Library | Target | What it is | Page |
|---|---|---|---|
| `can` | `can` | The backend-independent half: the `Channel` interface, channel naming, bit-timing arithmetic, the FD length table, a virtual backend. | not written yet |
| `can_pcan` | `can_pcan` | PCAN-USB FD adapters over libusb. | not written yet |
| `can_socketcan` | `can_socketcan` | The kernel's CAN stack. Linux at runtime, built everywhere. | not written yet |
| `can_motec` | `can_motec` | The MoTeC UTC over libusb, and the network gateways that speak the same command envelope. | [can_motec](can_motec.html) |
| `can_trc` | `can_trc` | PCAN `.trc` traces, read and written; a trace as a replay channel. | not written yet |
| `can_backends` | `can_backends` | Assembles the backends into one registry so `can` need not know they exist. | not written yet |
| `dbc_parser` | `dbc_parser` | DBC files parsed, and typed decoders generated from them at build time. | not written yet |
| `canopen` | `canopen` | EDS parsing, SDO, PDO, NMT and LSS, a virtual bus, and code generation from an EDS. | not written yet |

## Devices

| Library | Target | What it is | Page |
|---|---|---|---|
| `gsof` | `gsof` | The Trimble GSOF protocol. `constexpr`, no I/O. | not written yet |
| `bd992` | `bd992` | TCP, reconnection and read-before-write configuration for a BD992. | not written yet |
| `xbus` | `xbus` | The Xsens XBus protocol. `constexpr`, no I/O. | not written yet |
| `mti610` | `mti610` | The serial port, the reader thread and the Config/Measurement handshake for an MTi-610. | not written yet |
| `mototrbo` | `mototrbo` | MOTOTRBO XNL, XCMP and NAI. `constexpr`, no I/O. | not written yet |
| `xpr` | `xpr` | A MOTOTRBO radio's session over TCP. | not written yet |
| `msel` | `msel` | The MSEL Master Relay protocol: decoding reports, building commands. | not written yet |
| `display_backlight` | `display_backlight` | A display module's rootfs record and the sysfs backlight, light sensor and temperature devices behind it. | not written yet |
| `i2c_bus` | `i2c_bus` | An I2C bus by host: Linux `i2c-dev`, or an MCP2221A bridge elsewhere. | not written yet |
| `mcp2221a` | `mcp2221a` | The Microchip USB-to-I2C bridge, over HID. | not written yet |

## Apple and CarPlay

| Library | Target | What it is | Page |
|---|---|---|---|
| `plist` | `plist` | Apple property lists, binary and XML. | not written yet |
| `apple_usb` | `apple_usb` | usbmux client and server, lockdown, TLS, pairing, NCM discovery. Portable protocol layer, Linux transport. | not written yet |
| `apple_mfi_ic` | `apple_mfi_ic` | The MFi authentication coprocessor over I2C. | not written yet |
| `iap2` | `iap2` | The iAP2 accessory protocol and the MFi signer. | not written yet |
| `airplay` | `airplay` | The CarPlay session: RTSP control, pairing crypto, H.264 video, AAC audio, HID. | not written yet |

## Map

| Library | Target | What it is | Page |
|---|---|---|---|
| `protowire` | `protowire` | Just enough protobuf for MVT and OSM PBF. | not written yet |
| `osm` | `osm` | OpenStreetMap PBF, decoded. | not written yet |
| `mvt` | `mvt` | Mapbox Vector Tiles, decoded and encoded. | not written yet |
| `mbtiles` | `mbtiles` | The `.mbtiles` archive format, and the one place TMS rows become XYZ. | not written yet |
| `map_rules` | `map_rules` | What a piece of OSM data is, decided once for the tiler and the router alike. | not written yet |
| `map_wire` | `map_wire` | A road-graph segment on the wire. | not written yet |
| `road_graph` | `road_graph` | The routable road network on disk, mmap'd, with a contraction hierarchy. | not written yet |
| `track_store` | `track_store` | The race-track catalogue that lives inside the `.mbtiles`. | not written yet |
| `map_render` | `map_render` | Vector tiles to pixels through QRhi against an offscreen texture; labels, tile cache. | not written yet |
| `map_surface` | `map_surface` | The same map pass drawn straight into a `QRhiWidget`. | not written yet |
| `map_controls` | `map_controls` | The floating buttons both map surfaces overlay on their maps. | not written yet |
