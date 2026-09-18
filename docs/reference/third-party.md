---
title: Third-party libraries
parent: Reference
nav_order: 2
---

# Third-party libraries

Everything below is fetched and built by CMake during the configure step; none
of it needs to be installed first. The pins live in `third_party/*.cmake`, one
file per dependency, and this table is checked against them. Each dependency's
licence text is copied into `build/licenses/<name>/` by the same step, with a
`fetch_info.txt` recording what was fetched from where.

Qt 6 and FFmpeg are the exceptions: they are found on the system, not fetched.
So are OpenSSL and zlib.

| Library | Version | Licence | What it is used for | Patched |
|---|---|---|---|---|
| [Qt 6](https://www.qt.io/) | system | LGPL v3 / commercial | GUI, multimedia, the RHI the map renders through | no |
| [FFmpeg](https://ffmpeg.org/) | system | LGPL v2.1+ | H.264/HEVC and AAC decoding for CarPlay and scope's video panel | no |
| [capnproto](https://github.com/capnproto/capnproto) | v1.5.0 | MIT | Every message on the bus | no |
| [zenoh](https://github.com/eclipse-zenoh/zenoh) | release/1.10.0 | EPL-2.0 / Apache-2.0 | The pub/sub and query transport (Rust crate, built by cargo) | yes: `patches/zenoh_abortable_gossip_connect.patch` |
| [zenoh-c](https://github.com/eclipse-zenoh/zenoh-c) | 1.10.0 | EPL-2.0 / Apache-2.0 | The C API over the crate | no |
| [zenoh-cpp](https://github.com/eclipse-zenoh/zenoh-cpp) | 1.10.0 | EPL-2.0 / Apache-2.0 | The C++ binding the tree uses | no |
| [zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) | 1.10.0 | EPL-2.0 / Apache-2.0 | The browser's zenoh client, compiled to wasm for the web console. Never linked on the board -- that is zenoh-c's job | yes: `patches/zenoh_pico_emscripten_stddef.patch`, `patches/zenoh_pico_emscripten_ws_closed_link.patch` |
| [spdlog](https://github.com/gabime/spdlog) | v1.17.0 | MIT | Logging | yes: `patches/spdlog_tweakme.patch` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (`55f9368`) | MIT | JSON for configs, the agent interface and tool output | no |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.9.0 | MIT | Layout and node configuration files | no |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 3.3.1 (`44380e5`) | MIT | Command-line parsing | no |
| [exprtk](https://github.com/ArashPartow/exprtk) | `1e4a80b` | MIT | The expression parser behind every data source | no |
| [libusb-cmake](https://github.com/libusb/libusb-cmake) | v1.0.27 | LGPL v2.1+ | PCAN and MoTeC CAN adapters, the CarPlay USB path | no |
| [hidapi](https://github.com/libusb/hidapi) | 0.15.0 (`d6b2a97`) | BSD-3 / GPL v3 (dual) | The MCP2221A USB-to-I2C bridge | no |
| [mcap](https://github.com/foxglove/mcap) | releases/cpp/v2.1.3 | MIT | The recording format `bag` writes and reads | no |
| [lz4](https://github.com/lz4/lz4) | v1.10.0 | BSD-2 | MCAP chunk compression | no |
| [zstd](https://github.com/facebook/zstd) | v1.5.7 | BSD-3 / GPL v2 (dual) | MCAP chunk compression | no |
| [sqlite3](https://www.sqlite.org/) | 3.53.4 (amalgamation) | public domain | `.mbtiles` archives | no |
| [earcut.hpp](https://github.com/mapbox/earcut.hpp) | v3.2.3 | ISC | Polygon tessellation for map tiles | no |
| [lexy](https://github.com/foonathan/lexy) | v2025.05.0 | BSL-1.0 | The DBC and EDS parsers | no |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.56.0 | MIT | The web console's HTTP server. Header-only, and its ContentReader streams a ~338 MB RAUC bundle to disk instead of buffering it | no |

The dashboard itself is GPL-3.0-or-later; see `COPYING` and `NOTICE` in the
repository. The CarPlay stack was ported from
[LIVI](https://github.com/f-io/LIVI), also GPL-3.0-or-later.
