# Redline Labs Digital Dash

A modern Qt6-based dashboard application.  Part of a larger project to create an embedded Linux distribution that will run on custom hardware.   The goal is to have a hackable digital dash for vehicles that people can use out of the box with a high amount of customization, or get their hands dirty and extend it to add whatever functionality they desire.

## Features

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
   ./carplay_cpp
   ```

### Command Line Options

- `--debug`: Enable debug logging
- `--libusb_debug`: Enable LibUSB debugging logging

### Configuration

The application uses `config.yaml` for configuration. Key settings include:

- Display resolution and DPI settings
- CarPlay frame intervals
- Audio buffer sizes
- Night mode and drive type preferences

## Third-Party Libraries

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| [Qt6](https://www.qt.io/) | 6.x | LGPL v3 / Commercial | GUI framework and multimedia support |
| [spdlog](https://github.com/gabime/spdlog) | 1.15.3 | MIT | Fast C++ logging library |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON parsing and serialization |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | MIT | YAML configuration file parsing |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 3.3.1 | MIT | Command line argument parsing |
| [libusb](https://github.com/libusb/libusb) | 1.0.27 | LGPL v2.1+ | USB device communication |

### Patches
- **spdlog**: Configuration (tweakme.h) for the default logger.


## Project Structure

```
mercedes_dashboard/
├── main.cpp                    # Application entry point
├── config.yaml                 # Configuration file
├── CMakeLists.txt             # Build configuration
├── widgets/                   # Custom widget implementations
│   ├── carplay/              # CarPlay integration
│   ├── mercedes_190e_*       # Mercedes-specific widgets
│   └── sparkline/            # Data visualization
├── resources/                 # Application resources
├── cmake/                     # Dependency configurations
└── patches/                  # Third-party patches
```

## License

See `COPYING` for license information. 