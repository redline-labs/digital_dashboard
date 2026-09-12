cmake_minimum_required(VERSION 3.10)

project(mti610)

# Talking to an Xsens MTi-610 over a serial port.
#
# The split from `xbus` is the same one `bd992` makes from `gsof`: bytes on one
# side, transports on the other. Everything here owns a file descriptor, a
# thread or a file; nothing here knows a field offset. That is what lets the
# protocol half be constexpr and checked at compile time -- which matters more
# here than it did for GSOF, because there is no MTi on the bench to disagree.
#
# THIS IS THE FIRST SERIAL CODE IN THE TREE. Every other device here is TCP,
# libusb, hidapi or SocketCAN. It is termios rather than libusb deliberately:
# an MTi-600 development kit presents as a USB-serial adapter, so the whole
# stack builds and runs on macOS as well as on the vehicle.
#
# Deliberately free of zenoh and capnp too. The node maps items onto schemas; a
# library that knew what an `XbusAcceleration` was could not be tested without
# a bus.
add_library(mti610 STATIC
    src/device_session.cpp
    src/error.cpp
    src/output_config.cpp
    src/replay_stream.cpp
    src/serial_stream.cpp
    src/stream_client.cpp
)

target_include_directories(mti610 PUBLIC include)

target_link_libraries(mti610
    PUBLIC
        xbus
    PRIVATE
        spdlog::spdlog
)

# The desired-versus-actual comparison that decides whether the node touches
# the device's stored settings at all. Pure logic -- no port, no device --
# which is the whole reason it was written as free functions over two lists.
add_executable(mti610_test_output_config
    tests/test_output_config.cpp
)

target_link_libraries(mti610_test_output_config
    PRIVATE
        mti610
        spdlog::spdlog
)

add_project_test(TARGET mti610_test_output_config LABELS mti610 unit)

# The serial port, the session handshake and the reader thread, against a
# scripted MTi on the other end of a pty. No hardware, but a real tty with real
# termios settings and real partial reads -- which is what makes it worth more
# than a mock.
add_executable(mti610_test_stream
    tests/test_stream.cpp
)

target_link_libraries(mti610_test_stream
    PRIVATE
        mti610
        spdlog::spdlog
)

add_project_test(TARGET mti610_test_stream LABELS mti610 unit)
