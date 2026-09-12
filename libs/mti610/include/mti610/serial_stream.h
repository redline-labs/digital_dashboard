// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ByteStream over a serial port.
//
// THIS IS THE FIRST SERIAL CODE IN THIS TREE. Every other device here speaks
// TCP (bd992, xpr), libusb (can_pcan, can_motec, apple_usb), hidapi (mcp2221a)
// or SocketCAN. An MTi-600 development kit presents as a USB-serial adapter,
// so termios is the right layer -- and because it is termios rather than
// libusb, the whole stack builds and runs on macOS as well as on the vehicle.
//
// Three decisions worth stating, because each one is a bug that does not
// announce itself:
//
//   * RAW MODE, VIA cfmakeraw. A port left in canonical mode buffers until a
//     newline and translates CR to LF on the way through. XBus is binary: a
//     payload byte of 0x0D would be rewritten in transit and the message would
//     fail its checksum, once in every 256 bytes, with nothing in the logs but
//     a resync count.
//
//   * VMIN = 0, VTIME = 0, with poll() doing the waiting. Letting termios time
//     out instead gives a resolution of a tenth of a second and no way to
//     distinguish "nothing yet" from "port gone". poll() gives both.
//
//   * THE PORT IS OPENED O_NONBLOCK, and O_NOCTTY. Without O_NONBLOCK, open()
//     on a serial device blocks until carrier is asserted, which on an adapter
//     with no device behind it is forever and is not interruptible. Without
//     O_NOCTTY the port can become the process's controlling terminal, and a
//     hangup on it then signals the process.

#ifndef MTI610_SERIAL_STREAM_H
#define MTI610_SERIAL_STREAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "mti610/byte_stream.h"
#include "mti610/error.h"

namespace mti610
{

// The baud rates an MTi-600 serial port can be set to, as termios speed
// constants. The device's own baud-rate BYTE is a different encoding entirely
// (LLCP Table 7, where 115200 is 0x02 and 921600 is 0x80) and is not used
// here: this library never changes the device's baud rate, because doing so
// needs SetPortConfig, whose word layout the LLCP documents only as an image.
// See docs/mti610.md.
//
// 115200 is the factory default in serial mode.
bool is_supported_baud(unsigned baud);

class SerialStream final : public ByteStream
{
  public:
    struct Options
    {
        unsigned baud { 115200 };

        // Wait this long for the port to appear before giving up. A USB-serial
        // adapter that has just been plugged in can take a moment to
        // enumerate, and failing instantly would turn a normal plug-in into a
        // reopen cycle.
        unsigned openTimeoutMs { 2000 };
    };

    // Opens `path` (/dev/ttyUSB0 on Linux, /dev/cu.usbserial-XXXX on macOS)
    // and puts it in raw mode at the requested baud.
    static Result<std::unique_ptr<SerialStream>> open(const std::string& path, Options options);

    ~SerialStream() override;

    bool sendAll(std::span<const std::uint8_t> data) override;
    ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) override;
    bool isOpen() const override;
    void close() override;

    const std::string& path() const { return mPath; }
    int fd() const { return mFd; }

    // Discard anything the driver has buffered in either direction.
    //
    // Called after entering Config state, and the reason is specific: the
    // device may have been mid-MTData2 when GoToConfig arrived, so the first
    // bytes after the acknowledgement can be the tail of a measurement. The
    // framer would resync past them, but a command/response exchange that read
    // them as its answer would not.
    void flush();

  private:
    SerialStream(int fd, std::string path);

    int mFd { -1 };
    std::string mPath;
};

// For tests: wrap a file descriptor this class did not open, such as one end
// of a pty pair. Takes ownership.
std::unique_ptr<ByteStream> adopt_fd(int fd, std::string label);

} // namespace mti610

#endif // MTI610_SERIAL_STREAM_H
