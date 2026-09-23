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
| `pub_sub` | `zenoh_pub_sub` | Zenoh sessions, Cap'n Proto publish and subscribe, expression subscriptions, services, liveliness; the generated schema registry. | [pub_sub](pub_sub.html) |
| `reflection` | `reflection` | `REFLECT_STRUCT` and `REFLECT_ENUM`: the compile-time field lists every codec and UI walks. | [reflection](reflection.html) |
| `config_codec` | `config_codec` | Reflected-struct configuration: YAML and JSON both ways, self-description, validation by field path, clamping. No Qt. | [config_codec](config_codec.html) |
| `core` | `core` | Paths, logging setup, systemd notify; the one place the runtime environment variables are read. | [core](core.html) |
| `cli` | `cli` | Verb dispatch for the multi-verb tools: global options, exit codes, results on stdout. | [cli](cli.html) |
| `helpers` | `helpers` | Qt-free odds and ends: hex, colour, unit conversion, a rate gate, the FFmpeg log bridge. | [helpers](helpers.html) |
| `qt_helpers` | `qt_helpers` | The Qt-using sibling: a paint-caching widget base, colour conversion, resource fonts. | [qt_helpers](qt_helpers.html) |
| `agent_control` | `agent_control` | The `--mcp` control socket embedded in every GUI app: methods, screenshots, input, the widget locator. | [agent_control](agent_control.html) |
| `dashboard_widgets` | `dashboard_widgets` | Every dashboard widget, the widget table, the layout config and its loader, the factory, and the `widget.*` agent methods; shared by the dashboard and the editor. | [dashboard_widgets](dashboard_widgets.html) |
| `node_health` | `node_health` | The health a node publishes and the monitor that reads every node at once; what `inspect health` prints. | [node_health](node_health.html) |
| `bag` | `bag` | Recording and replaying the bus as MCAP. | [bag](bag.html) |

## CAN

| Library | Target | What it is | Page |
|---|---|---|---|
| `can` | `can` | The backend-independent half: the `Channel` interface, channel naming, bit-timing arithmetic, the FD length table, a virtual backend. | [can](can.html) |
| `can_pcan` | `can_pcan` | PCAN-USB FD adapters over libusb. | [can_pcan](can_pcan.html) |
| `can_socketcan` | `can_socketcan` | The kernel's CAN stack. Linux at runtime, built everywhere. | [can_socketcan](can_socketcan.html) |
| `can_motec` | `can_motec` | The MoTeC UTC over libusb, and the network gateways that speak the same command envelope. | [can_motec](can_motec.html) |
| `can_trc` | `can_trc` | PCAN `.trc` traces, read and written; a trace as a replay channel. | [can_trc](can_trc.html) |
| `can_backends` | `can_backends` | Assembles the backends into one registry so `can` need not know they exist. | [can_backends](can_backends.html) |
| `dbc_parser` | `dbc_parser` | DBC files parsed, and typed decoders generated from them at build time. | [dbc_parser](dbc_parser.html) |
| `canopen` | `canopen` | EDS parsing, SDO, PDO, NMT and LSS, a virtual bus, and code generation from an EDS. | [canopen](canopen.html) |

## Devices

| Library | Target | What it is | Page |
|---|---|---|---|
| `gsof` | `gsof` | The Trimble GSOF protocol. `constexpr`, no I/O. | [gsof](gsof.html) |
| `bd992` | `bd992` | TCP, reconnection and read-before-write configuration for a BD992. | [bd992](bd992.html) |
| `xbus` | `xbus` | The Xsens XBus protocol. `constexpr`, no I/O. | [xbus](xbus.html) |
| `mti610` | `mti610` | The serial port, the reader thread and the Config/Measurement handshake for an MTi-610. | [mti610](mti610.html) |
| `mototrbo` | `mototrbo` | MOTOTRBO XNL, XCMP and NAI. `constexpr`, no I/O. | [mototrbo](mototrbo.html) |
| `xpr` | `xpr` | A MOTOTRBO radio's session over TCP. | [xpr](xpr.html) |
| `msel` | `msel` | The MSEL Master Relay protocol: decoding reports, building commands. | [msel](msel.html) |
| `display_backlight` | `display_backlight` | A display module's rootfs record and the sysfs backlight, light sensor and temperature devices behind it. | [display_backlight](display_backlight.html) |
| `i2c_bus` | `i2c_bus` | An I2C bus by host: Linux `i2c-dev`, or an MCP2221A bridge elsewhere. | [i2c_bus](i2c_bus.html) |
| `mcp2221a` | `mcp2221a` | The Microchip USB-to-I2C bridge, over HID. | [mcp2221a](mcp2221a.html) |

## Apple and CarPlay

| Library | Target | What it is | Page |
|---|---|---|---|
| `plist` | `plist` | Apple property lists, binary and XML. | [plist](plist.html) |
| `apple_usb` | `apple_usb` | usbmux client and server, lockdown, TLS, pairing, NCM discovery. Portable protocol layer, Linux transport. | [apple_usb](apple_usb.html) |
| `apple_mfi_ic` | `apple_mfi_ic` | The MFi authentication coprocessor over I2C. | [apple_mfi_ic](apple_mfi_ic.html) |
| `iap2` | `iap2` | The iAP2 accessory protocol and the MFi signer. | [iap2](iap2.html) |
| `airplay` | `airplay` | The CarPlay session: RTSP control, pairing crypto, H.264 video, AAC audio, HID. | [airplay](airplay.html) |

## Map

| Library | Target | What it is | Page |
|---|---|---|---|
| `protowire` | `protowire` | Just enough protobuf for MVT and OSM PBF. | [protowire](protowire.html) |
| `osm` | `osm` | OpenStreetMap PBF, decoded. | [osm](osm.html) |
| `mvt` | `mvt` | Mapbox Vector Tiles, decoded and encoded. | [mvt](mvt.html) |
| `mbtiles` | `mbtiles` | The `.mbtiles` archive format, and the one place TMS rows become XYZ. | [mbtiles](mbtiles.html) |
| `map_rules` | `map_rules` | What a piece of OSM data is, decided once for the tiler and the router alike. | [map_rules](map_rules.html) |
| `map_wire` | `map_wire` | A road-graph segment on the wire. | [map_wire](map_wire.html) |
| `road_graph` | `road_graph` | The routable road network on disk, mmap'd, with a contraction hierarchy. | [road_graph](road_graph.html) |
| `track_store` | `track_store` | The race-track catalogue that lives inside the `.mbtiles`. | [track_store](track_store.html) |
| `map_render` | `map_render` | Vector tiles to pixels through QRhi against an offscreen texture; labels, tile cache. | [map_render](map_render.html) |
| `map_surface` | `map_surface` | The same map pass drawn straight into a `QRhiWidget`. | [map_surface](map_surface.html) |
| `map_controls` | `map_controls` | The floating buttons both map surfaces overlay on their maps. | [map_controls](map_controls.html) |

## State estimation

| Library | Target | What it is | Page |
|---|---|---|---|
| `csym` | `csym` | Compile-time symbolic differentiation: a residual lambda traced, differentiated and lowered to straight-line code with its Jacobian, `JᵀJ` and `Jᵀr`. Header-only. | [csym](csym.html) |
| `geodesy` | `geodesy` | The WGS 84 ellipsoid: geodetic↔ECEF (Vermeille), the NED frame, radii, normal gravity and earth rate, templated so csym can differentiate through it. | [geodesy](geodesy.html) |
| `wmm` | `wmm`, `wmm_hr2025` | The World Magnetic Model: a compile-time `.COF` parser and field synthesis, with WMM-HR 2025 embedded and parsed during the build. | [wmm](wmm.html) |
| `factor_graph` | `factor_graph` | Nonlinear least squares on csym residuals: a fixed-lag smoother with frozen marginal priors, and a batch smoother that is RTS for linear models. | [factor_graph](factor_graph.html) |
| `imu_preint` | `imu_preint` | IMU preintegration from strapdown dq/dv in ECEF with earth rate; the IMU and bias-walk factors, the MTi sample sequencer, a truth simulator. | [imu_preint](imu_preint.html) |
| `vehicle_estimator` | `vehicle_estimator` | MTi and dual-antenna GNSS fused into position, attitude, velocity, acceleration and sideslip, with no bus types; the whole-drive smoother; `vehicle_estimator_sim`. | [vehicle_estimator](vehicle_estimator.html) |
