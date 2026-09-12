// SPDX-License-Identifier: GPL-3.0-or-later

#include "mti610/serial_stream.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
// macOS's termios stops at B230400. Anything above it is set by ioctl AFTER
// tcsetattr, which is the documented way and the only way -- there are no
// B460800 or B921600 constants to pass to cfsetspeed. Getting this wrong is
// silent: the port opens, runs at the last rate that WAS representable, and
// receives nothing but checksum errors.
#include <IOKit/serial/ioss.h>
#endif

namespace mti610
{
namespace
{

struct BaudEntry
{
    unsigned baud;
    speed_t speed;
};

// The rates both termios and an MTi-600 serial port can agree on. Rates above
// 921600 exist in the LLCP's table but have no portable termios constant, so
// they are absent rather than silently approximated -- a port opened at a rate
// the driver rounded is a port that receives nothing but checksum errors.
// The rates this library will open a port at.
//
// `speed` is the termios constant where one exists. Above B230400 macOS has
// none, so those rows carry kNeedsIoctl and are applied with IOSSIOSPEED after
// tcsetattr -- see applyBaud(). Linux has the constants and takes the ordinary
// path for all of them.
//
// The device's own baud-rate BYTE is a third encoding again (LLCP Table 7,
// where 115200 is 0x02 and 921600 is 0x80) and appears nowhere in this
// library: nothing here changes the device's baud rate, because that needs
// SetPortConfig, whose word layout the LLCP documents only as an image.
constexpr speed_t kNeedsIoctl = static_cast<speed_t>(-1);

constexpr std::array<BaudEntry, 8> kBaudTable {{
    { 9600, B9600 },
    { 19200, B19200 },
    { 38400, B38400 },
    { 57600, B57600 },
    { 115200, B115200 },
    { 230400, B230400 },
#ifdef B460800
    { 460800, B460800 },
#else
    { 460800, kNeedsIoctl },
#endif
#ifdef B921600
    { 921600, B921600 },
#else
    { 921600, kNeedsIoctl },
#endif
}};

const BaudEntry* findBaud(unsigned baud)
{
    for (const BaudEntry& entry : kBaudTable)
    {
        if (entry.baud == baud)
        {
            return &entry;
        }
    }
    return nullptr;
}

// A ByteStream over a descriptor this library did not open. Used by the tests
// to drive a pty, and deliberately not exposed as a way to hand SerialStream a
// socket -- the termios setup is half of what SerialStream is for.
class AdoptedFd final : public ByteStream
{
  public:
    AdoptedFd(int fd, std::string label) : mFd(fd), mLabel(std::move(label)) {}

    ~AdoptedFd() override { close(); }

    bool sendAll(std::span<const std::uint8_t> data) override;
    ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) override;
    bool isOpen() const override { return mFd >= 0; }

    void close() override
    {
        if (mFd >= 0)
        {
            ::close(mFd);
            mFd = -1;
        }
    }

  private:
    int mFd { -1 };
    std::string mLabel;
};

// The write loop both implementations share. A serial port at 115200 accepts
// about eleven bytes per millisecond, so a long write genuinely does come back
// short, and a caller that treated one write() as the whole message would
// truncate commands under load.
bool writeAll(int fd, std::span<const std::uint8_t> data, const std::string& what)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);

        if (n > 0)
        {
            sent += static_cast<std::size_t>(n);
            continue;
        }

        if (n < 0 && (errno == EINTR))
        {
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // The driver's buffer is full. Wait for room rather than spinning
            // or failing: at a low baud rate this is ordinary.
            struct pollfd pfd { fd, POLLOUT, 0 };
            const int ready = ::poll(&pfd, 1, 1000);
            if (ready > 0)
            {
                continue;
            }
            SPDLOG_WARN("mti610: {} write stalled with {} of {} bytes sent", what, sent,
                        data.size());
            return false;
        }

        SPDLOG_WARN("mti610: {} write failed: {}", what, std::strerror(errno));
        return false;
    }

    return true;
}

ssize_t readSome(int fd, std::span<std::uint8_t> out, unsigned timeoutMs, const std::string& what)
{
    struct pollfd pfd { fd, POLLIN, 0 };
    const int ready = ::poll(&pfd, 1, static_cast<int>(timeoutMs));

    if (ready == 0)
    {
        return 0;
    }

    if (ready < 0)
    {
        if (errno == EINTR)
        {
            // A signal, not a failure. Reported as a timeout so a caller's
            // poll loop simply comes round again -- which is what makes
            // SIGINT-driven shutdown work without a special case here.
            return 0;
        }
        SPDLOG_WARN("mti610: {} poll failed: {}", what, std::strerror(errno));
        return -1;
    }

    // POLLHUP on a serial port means the adapter was unplugged. There may
    // still be buffered bytes behind it, so it is not handled before the read:
    // the read returning 0 is what settles it.
    const ssize_t n = ::read(fd, out.data(), out.size());

    if (n > 0)
    {
        return n;
    }

    if (n == 0)
    {
        // End of file on a tty means the other side is gone. Distinct from the
        // timeout above, and the distinction is what stops a reader thread
        // spinning on a dead port.
        return -1;
    }

    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
    {
        return 0;
    }

    SPDLOG_WARN("mti610: {} read failed: {}", what, std::strerror(errno));
    return -1;
}

bool AdoptedFd::sendAll(std::span<const std::uint8_t> data)
{
    return mFd >= 0 && writeAll(mFd, data, mLabel);
}

ssize_t AdoptedFd::recvSome(std::span<std::uint8_t> out, unsigned timeoutMs)
{
    if (mFd < 0)
    {
        return -1;
    }
    return readSome(mFd, out, timeoutMs, mLabel);
}

} // namespace

bool is_supported_baud(unsigned baud)
{
    return findBaud(baud) != nullptr;
}

SerialStream::SerialStream(int fd, std::string path) : mFd(fd), mPath(std::move(path)) {}

SerialStream::~SerialStream()
{
    close();
}

Result<std::unique_ptr<SerialStream>> SerialStream::open(const std::string& path, Options options)
{
    const BaudEntry* baud = findBaud(options.baud);
    if (baud == nullptr)
    {
        return invalid_argument("unsupported baud rate " + std::to_string(options.baud) +
                                "; supported: 9600, 19200, 38400, 57600, 115200, 230400, "
                                "460800, 921600");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.openTimeoutMs);

    int fd = -1;
    int lastErrno = 0;

    // Retry until the deadline. A USB-serial adapter that was just plugged in
    // exists in /dev a moment before it is ready, and failing on the first
    // ENOENT would turn an ordinary plug-in into a full reopen cycle.
    while (true)
    {
        fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0)
        {
            break;
        }

        lastErrno = errno;

        if (std::chrono::steady_clock::now() >= deadline)
        {
            if (lastErrno == ENOENT)
            {
                return not_found(path + " does not exist");
            }
            return open_failed("cannot open " + path, lastErrno);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    struct termios tty {};
    if (::tcgetattr(fd, &tty) != 0)
    {
        const int saved = errno;
        ::close(fd);
        // A descriptor that opened but is not a tty is nearly always a path
        // typo that landed on a regular file, which would otherwise present as
        // a device that never says anything.
        return open_failed(path + " is not a serial port", saved);
    }

    // Raw mode: no canonical buffering, no echo, no CR/LF translation, no
    // parity or flow control fiddling with the bytes. See the header comment.
    ::cfmakeraw(&tty);

    // For a rate with a termios constant this is the whole story. For one
    // without, a representable placeholder goes in now and the real rate is
    // applied by ioctl after tcsetattr -- IOSSIOSPEED operates on a port that
    // is already configured.
    const speed_t placeholder = (baud->speed == kNeedsIoctl) ? B115200 : baud->speed;

    if (::cfsetispeed(&tty, placeholder) != 0 || ::cfsetospeed(&tty, placeholder) != 0)
    {
        const int saved = errno;
        ::close(fd);
        return open_failed("cannot set baud rate on " + path, saved);
    }

    // 8N1, no hardware or software flow control. An MTi uses none: asserting
    // RTS/CTS against a device that does not drive them stalls the port
    // forever, and XON/XOFF would eat payload bytes 0x11 and 0x13.
    tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= static_cast<tcflag_t>(CS8);
    tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
    tty.c_cflag |= static_cast<tcflag_t>(CREAD | CLOCAL);
    tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);

    // poll() does the waiting, so termios must not. See the header comment.
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        const int saved = errno;
        ::close(fd);
        return open_failed("cannot configure " + path, saved);
    }

    if (baud->speed == kNeedsIoctl)
    {
#ifdef __APPLE__
        speed_t rate = options.baud;
        if (::ioctl(fd, IOSSIOSPEED, &rate) == -1)
        {
            const int saved = errno;
            ::close(fd);
            return open_failed("cannot set " + std::to_string(options.baud) + " baud on " + path,
                               saved);
        }
#else
        // Reached only if a platform has neither the constant nor the ioctl.
        // Refused rather than silently left at the placeholder, because a port
        // running at the wrong rate receives nothing but checksum errors.
        ::close(fd);
        return invalid_argument(std::to_string(options.baud) +
                                " baud is not settable on this platform");
#endif
    }

    // Anything the driver buffered before we configured it was read under the
    // old settings and cannot be trusted.
    ::tcflush(fd, TCIOFLUSH);

    SPDLOG_INFO("mti610: opened {} at {} baud", path, options.baud);

    return std::unique_ptr<SerialStream>(new SerialStream(fd, path));
}

bool SerialStream::sendAll(std::span<const std::uint8_t> data)
{
    return mFd >= 0 && writeAll(mFd, data, mPath);
}

ssize_t SerialStream::recvSome(std::span<std::uint8_t> out, unsigned timeoutMs)
{
    if (mFd < 0)
    {
        return -1;
    }
    return readSome(mFd, out, timeoutMs, mPath);
}

bool SerialStream::isOpen() const
{
    return mFd >= 0;
}

void SerialStream::flush()
{
    if (mFd >= 0)
    {
        ::tcflush(mFd, TCIOFLUSH);
    }
}

void SerialStream::close()
{
    if (mFd >= 0)
    {
        ::close(mFd);
        mFd = -1;
    }
}

std::unique_ptr<ByteStream> adopt_fd(int fd, std::string label)
{
    return std::make_unique<AdoptedFd>(fd, std::move(label));
}

} // namespace mti610
