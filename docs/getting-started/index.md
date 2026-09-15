---
title: Getting started
nav_order: 1
---

# Getting started

Everything in the tree builds from one CMake project: the four GUI apps, every
node, the libraries and their tests. This page gets you from a clone to a
dashboard on screen and a passing test run.

## Prerequisites

| | |
|---|---|
| CMake | 3.16 or newer |
| Compiler | C++23: recent clang or GCC |
| Qt 6 | Core, Gui, Widgets, Svg, Multimedia, GuiPrivate, ShaderTools |
| FFmpeg | libavcodec and libavutil, found through pkg-config |
| OpenSSL, zlib | the Apple MFi and CarPlay stack, and compressed tiles |
| Rust toolchain | `cargo`; the zenoh transport is a Rust crate built during the CMake step |
| pkg-config, git | |

On macOS with Homebrew:

```bash
brew install cmake qt@6 ffmpeg openssl pkg-config rustup
rustup-init -y
```

Every other dependency (capnproto, zenoh, spdlog, yaml-cpp, nlohmann/json,
cxxopts, libusb, hidapi, exprtk, mcap, sqlite3, and the rest) is fetched and
built by CMake. The full list with versions and licences is on the
[third-party](../reference/third-party.html) page.

## Build

```bash
git clone https://github.com/redline-labs/digital_dashboard.git
cd digital_dashboard
cmake -S . -B build
cmake --build build -j8
```

The first configure fetches the third-party sources and builds the zenoh crate,
so it takes a while. After that a rebuild is incremental.

Binaries land in `build/` mirroring the source tree: `build/apps/dashboard/dashboard`,
`build/apps/scope/scope`, `build/nodes/inspect/inspect`, and so on. On macOS the
editor is an app bundle, `build/apps/editor/editor.app`.

## Run a demo layout

```bash
./build/apps/dashboard/dashboard -c configs/dashboard/mercedes_190e_dash.yaml
```

The gauges sit at rest because nothing is publishing. Put some data on the bus
from a second terminal:

```bash
./build/mock_data/test_data_publisher
```

`configs/dashboard/` holds the other layouts (two MoTeC clusters, a CarPlay
window, a map). They are all the same binary with a different file.

## Run the tests

```bash
ctest --test-dir build -L unit          # fast and deterministic; run this always
ctest --test-dir build -L gui           # constructs Qt widgets, forced offscreen
ctest --test-dir build -LE slow         # everything that finishes quickly
```

`-DBUILD_TESTING=OFF` at configure time drops the test targets out of the
default build.

## Where next

- Running the cluster, editing a layout, watching signals: [Apps](../apps/).
- Putting a piece of hardware on the bus: [Nodes](../nodes/).
- Driving the apps headless from an agent: [Agent control](../developing/agent-control.html).
- The runtime environment variables and data directories:
  [Runtime environment](../reference/environment.html).
