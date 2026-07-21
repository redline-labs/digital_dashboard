# Redline Labs Digital Dash

A modern Qt6-based dashboard application.  Part of a larger project to create an embedded Linux distribution that will run on custom hardware.   The goal is to have a hackable digital dash for vehicles that people can use out of the box with a high amount of customization, or get their hands dirty and extend it to add whatever functionality they desire.

All the gauges are drawn/rendered, no static images.  A YAML configuration file controls:
 - the composition of which widgets are shown and where.
 - Widget specific configurations, such as max RPM or where shift points are shown.
 - Data sources for each element within the widget.

 Zenoh is used for pub/sub, and capnproto is used for serialization.  The widgets additionally have a flexible "expression parser" to where a user can specify a capnproto signal and any math expression to be used for the data source for a widget.

For the examples shown below, they are all the same `dashboard` binary, only different YAML configurations.  Multiple windows (displays) can also be supported in the same single configuration - for example, you can have one display be your instrument cluster, the other be a CarPlay window.

#### (Work in progress) Mercedes 190E Instrument Cluster
With additional "sparklines" widget on the right.
![Screengrab](/docs/images/mercedes_190e_demo_display.png)

#### (Work in Progress) Motec CDL2
![Screengrab](/docs/images/motec_cdl2_demo.png)

#### (Work in Progress) Motec C125
![Screengrab](/docs/images/motec_c125_dash_demo.png)

### (Work in Progress) CarPlay Window.
Native wired CarPlay (no dongle required): the `carplay` driver node (Linux-only) talks to the iPhone directly over USB — Apple MFi authentication coprocessor required — and streams video/audio/metadata over zenoh to the dashboard widgets.  Ported to C++ from the [LIVI](https://github.com/f-io/LIVI) project (GPL-3.0-or-later).
![Screengrab](/docs/images/carplay_demo.png)

## Dashboard Editor
A very much work-in-progress dashboard editor is being pieced together.  This is a GUI to be able to drag and drop widgets into a dashboard.

![Screengrab](/docs/images/dashboard_editor.png)

## Documentation
Work in progress, see [dashboard-docs](http://dashboard-docs.redline-labs.com).

## Building Instructions

### Prerequisites

- **Qt6**: Core, Widgets, Multimedia, MultimediaWidgets, and Svg modules
- **FFmpeg**
- **CMake**: Version 3.10 or higher
- **C++ Compiler**: Supporting C++23 standard

### macOS Setup

```bash
# Install Qt6 via Homebrew
brew install qt@6
```

### Build Steps

1. **Create build directory**:
   ```bash
   mkdir build
   cd build
   ```

2. **Configure the project**:
   ```bash
   cmake ..
   ```
   
   For debug builds:
   ```bash
   cmake -DCMAKE_BUILD_TYPE=Debug ..
   ```

3. **Build the application**:
   ```bash
   make -j$(nproc)
   ```

4. **Run the application**:
   ```bash
   ./dashboard/dashboard -c <path_to_config>
   ```

### Command Line Options

- `-c`: Path to YAML configuration file (see config directory for examples)
- `--debug`: Enable debug logging

### Native Wired CarPlay

CarPlay runs as a separate driver node that owns the USB/iAP2/AirPlay session with the
iPhone and publishes video, audio, input and metadata over zenoh.  The dashboard widgets
are thin subscribers, so supplemental widgets (e.g. `now_playing`) can consume CarPlay
data without touching the projection stack.

```bash
# terminal 1 - the driver (Linux only; needs USB + TUN privileges)
sudo ./nodes/carplay/carplay --verbose

# terminal 2 - the dashboard
./dashboard/dashboard -c ../configs/dashboard/carplay_demo.yaml
```

No hardware handy?  `--simulate` publishes a synthetic session (H.264 test pattern, audio
tone, rotating metadata) on the real topics so the whole dashboard side can be exercised
on any platform:

```bash
./nodes/carplay/carplay --simulate
```

Hardware bring-up is documented step-by-step in [docs/carplay_bringup.md](docs/carplay_bringup.md).
Native CarPlay requires an Apple MFi authentication coprocessor (see `libs/apple_mfi_ic`)
and the `libimobiledevice`/`libplist` development packages.


## Third-Party Libraries
The following libraries are fetched as part of a CMake step.  For complete license information, see the individual license files in the resulting `build/licenses/` directory. Patches (where applicable) are also available in the source directory under the `patches` directory, and also copied out to the `build/licenses` directory.

| Library | Version | License | Purpose | Patched |
|---------|---------|---------|---------|---------|
| [Qt6](https://www.qt.io/) | 6.x | LGPL v3 / Commercial | GUI framework and multimedia support | N |
| [spdlog](https://github.com/gabime/spdlog) | 1.15.3 | MIT | Fast C++ logging library | Y |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON parsing and serialization | N|
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | MIT | YAML configuration file parsing | N |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 3.3.1 | MIT | Command line argument parsing | N |
| [capnproto](https://github.com/capnproto/capnproto) | 6846dff | MIT | Binary serialization and RPC framework | N |
| [libusb-cmake](https://github.com/libusb/libusb-cmake) | 1.0.27 | LGPL v2.1+ | USB device communication | N |
| [zenoh-cpp](https://github.com/eclipse-zenoh/zenoh-cpp) | 1.4.0 | EPL-2.0 / Apache-2.0 | Zero overhead pub/sub messaging | N |
| [hidapi](https://github.com/libusb/hidapi.git) | 0.15.0 | GPL3 | HID library | N |
| [exprtk](https://github.com/ArashPartow/exprtk) | 245c0d5 | MIT | Mathematical expression parsing and evaluation | Y |

## License

See `COPYING` for license information.
