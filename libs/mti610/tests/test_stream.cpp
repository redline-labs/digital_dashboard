// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serial port, the session handshake and the reader thread, against a
// scripted MTi on the other end of a pty.
//
// A pty is worth more here than a mock object would be. It is a real tty, so
// the termios setup runs for real: raw mode, VMIN/VTIME, the non-blocking open
// and the poll() loop are all exercised rather than stubbed. Reads come back
// partial the way they do on a real port, and closing the far end produces the
// same end-of-file a device being unplugged produces. What it cannot test is
// the baud rate, which a pty ignores -- that is on the hardware list in
// docs/mti610.md.
//
// The pattern -- bind in the constructor so the address is known before
// anything connects, serve on a background thread, tear down in the destructor
// -- is lifted from libs/bd992/tests/test_stream.cpp, which credits
// libs/apple_usb/test_usbmux_client.cpp for it.
//
// THE FAKE DEVICE ENFORCES THE PROTOCOL RATHER THAN ASSUMING IT. It answers
// only messages addressed with a valid BID, only in the state the LLCP says
// each message is valid in, and it refuses to answer a configuration message
// while in Measurement. Each of those is a rule this library has to obey and
// none of them fails loudly if it does not -- a device that simply does not
// answer is what a wrong assumption looks like, which is the same argument
// libs/xpr's fake radio makes.

#include "mti610/device_session.h"
#include "mti610/replay_stream.h"
#include "mti610/serial_stream.h"
#include "mti610/stream_client.h"
#include "xbus/commands.h"
#include "xbus/framer.h"
#include "xbus/mtdata2.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <mutex>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>  // openpty
#else
#include <pty.h>  // openpty
#endif
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// A scripted MTi on one end of a pty.
// ---------------------------------------------------------------------------

class FakeMti
{
  public:
    struct Behaviour
    {
        // What ReqOutputConfiguration reports before anything is written.
        std::vector<xbus::OutputEntry> outputs;

        // Answer GoToConfig? A device at the wrong baud rate answers nothing,
        // and that has to present as a timeout rather than as a hang.
        bool answerGoToConfig { true };

        // Clamp every requested rate to this, the way a device whose link
        // cannot carry the configuration does. Zero leaves rates alone.
        std::uint16_t clampHz { 0 };

        // Refuse SetOutputConfiguration with an Error message.
        bool refuseWrite { false };

        // Emit an unsolicited WakeUp after this many data messages, as a
        // brown-out would. Zero never does.
        unsigned resetAfterData { 0 };

        // Send this many MTData2 messages once in Measurement.
        unsigned dataMessages { 50 };
    };

    // Every field named, because -Wmissing-designated-field-initializers makes
    // a partial designated initialiser an error in this tree.
    static Behaviour defaults()
    {
        return Behaviour { .outputs = {},
                           .answerGoToConfig = true,
                           .clampHz = 0,
                           .refuseWrite = false,
                           .resetAfterData = 0,
                           .dataMessages = 50 };
    }

    explicit FakeMti(Behaviour behaviour) : mBehaviour(std::move(behaviour))
    {
        if (::openpty(&mHostFd, &mDeviceFd, nullptr, nullptr, nullptr) != 0)
        {
            SPDLOG_ERROR("openpty failed");
            return;
        }

        // The host side is what the library opens; raw mode on the device side
        // stops the tty line discipline echoing our commands back at us, which
        // would otherwise look like a device answering every message with
        // itself.
        makeRaw(mDeviceFd);
        makeRaw(mHostFd);

        mThread = std::thread(&FakeMti::run, this);
    }

    ~FakeMti()
    {
        stop();
    }

    FakeMti(const FakeMti&) = delete;
    FakeMti& operator=(const FakeMti&) = delete;

    void stop()
    {
        mRunning.store(false);
        if (mThread.joinable())
        {
            mThread.join();
        }
        closeFd(mDeviceFd);
        closeFd(mHostFd);
    }

    // The descriptor the library reads and writes. Handed to adopt_fd(), which
    // is the seam that lets this test drive the real code path without
    // SerialStream::open() needing a /dev node.
    int hostFd() const { return mHostFd; }

    bool valid() const { return mHostFd >= 0 && mDeviceFd >= 0; }

    std::vector<xbus::OutputEntry> written() const
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mWritten;
    }

    unsigned goToConfigCount() const { return mGoToConfig.load(); }
    unsigned goToMeasurementCount() const { return mGoToMeasurement.load(); }
    bool measuring() const { return mMeasuring.load(); }

  private:
    static void makeRaw(int fd)
    {
        struct termios tty {};
        if (::tcgetattr(fd, &tty) == 0)
        {
            ::cfmakeraw(&tty);
            tty.c_cc[VMIN] = 0;
            tty.c_cc[VTIME] = 0;
            ::tcsetattr(fd, TCSANOW, &tty);
        }
    }

    static void closeFd(int& fd)
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    void send(std::span<const std::uint8_t> bytes)
    {
        std::size_t sent = 0;
        while (sent < bytes.size() && mRunning.load())
        {
            const ssize_t n = ::write(mDeviceFd, bytes.data() + sent, bytes.size() - sent);
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            return;
        }
    }

    template <std::size_t N>
    void sendMessage(xbus::MessageId id, const std::array<std::uint8_t, N>& payload)
    {
        const auto message = xbus::make_message(id, payload);
        send(message);
    }

    void sendAck(xbus::MessageId ackId)
    {
        sendMessage(ackId, std::array<std::uint8_t, 0> {});
    }

    // One MTData2 carrying a sample: counter, sample time, status, and an
    // acceleration in fp16.32 -- the format the node's YAML asks for, chosen
    // so the swizzle is exercised end to end rather than only in a unit test.
    void sendData()
    {
        std::vector<std::uint8_t> body;

        const auto item = [&body](std::uint16_t rawId, std::span<const std::uint8_t> payload) {
            body.push_back(static_cast<std::uint8_t>(rawId >> 8));
            body.push_back(static_cast<std::uint8_t>(rawId & 0xFF));
            body.push_back(static_cast<std::uint8_t>(payload.size()));
            body.insert(body.end(), payload.begin(), payload.end());
        };

        const std::uint16_t counter = static_cast<std::uint16_t>(mCounter++);
        const std::array<std::uint8_t, 2> counterBytes {
            static_cast<std::uint8_t>(counter >> 8), static_cast<std::uint8_t>(counter & 0xFF)
        };
        item(0x1020, counterBytes);

        std::array<std::uint8_t, 4> ticks {};
        xbus::write_u32(ticks, 0, counter * 100u);
        item(0x1060, ticks);

        std::array<std::uint8_t, 4> status {};
        xbus::write_u32(status, 0, 0x00000003u);
        item(0xE020, status);

        std::array<std::uint8_t, 18> accel {};
        xbus::write_fp1632(accel, 0, 0.10);
        xbus::write_fp1632(accel, 6, -0.20);
        xbus::write_fp1632(accel, 12, 9.81);
        item(0x4022, accel);

        std::vector<std::uint8_t> message(body.size() + 5);
        const xbus::Result<std::size_t> used =
            xbus::encode_message(xbus::MessageId::MtData2, body, message);
        if (used)
        {
            send(std::span<const std::uint8_t>(message.data(), *used));
        }
    }

    void handle(const xbus::MessageView& message)
    {
        // A real MT only acknowledges a message addressed with a valid BID.
        // Enforced rather than assumed, because a library that used the wrong
        // one would see a device that simply never answers.
        if (message.bid != xbus::kBidMaster && message.bid != xbus::kBidFirstDevice)
        {
            return;
        }

        // Switched on the WIRE BYTE rather than on MessageId. A real device
        // answers a handful of the ids in xsxbusmessageid.h and ignores the
        // rest, so the domain here is genuinely 0..255 and a fallback is
        // correct -- which is not the same as dodging -Wswitch-enum on a
        // switch whose cases are all meaningful.
        switch (message.id)
        {
            case static_cast<std::uint8_t>(xbus::MessageId::GoToConfig):
                if (!mBehaviour.answerGoToConfig)
                {
                    return;
                }
                mMeasuring.store(false);
                ++mGoToConfig;
                sendAck(xbus::MessageId::GoToConfigAck);
                return;

            case static_cast<std::uint8_t>(xbus::MessageId::GoToMeasurement):
                // Config-state only. A device in Measurement ignores it.
                if (mMeasuring.load())
                {
                    return;
                }
                ++mGoToMeasurement;
                mMeasuring.store(true);
                mDataSent = 0;
                sendAck(xbus::MessageId::GoToMeasurementAck);
                return;

            case static_cast<std::uint8_t>(xbus::MessageId::ReqDeviceId):
                if (mMeasuring.load()) { return; }
                sendMessage(xbus::MessageId::DeviceId,
                            std::array<std::uint8_t, 8> { 0x00, 0x00, 0x00, 0x00,
                                                          0x03, 0xE8, 0x12, 0x34 });
                return;

            case static_cast<std::uint8_t>(xbus::MessageId::ReqProductCode):
                if (mMeasuring.load()) { return; }
                sendMessage(xbus::MessageId::ProductCode,
                            std::array<std::uint8_t, 14> { 'M', 'T', 'i', '-', '6', '1', '0',
                                                           'R', '-', '2', 'A', '5', 'G', '4' });
                return;

            case static_cast<std::uint8_t>(xbus::MessageId::ReqFirmwareRevision):
                if (mMeasuring.load()) { return; }
                sendMessage(xbus::MessageId::FirmwareRevision,
                            std::array<std::uint8_t, 11> { 0x01, 0x0A, 0x03, 0x00, 0x00, 0x04,
                                                           0xD2, 0x00, 0x00, 0x00, 0x01 });
                return;

            case static_cast<std::uint8_t>(xbus::MessageId::ReqHardwareVersion):
                if (mMeasuring.load()) { return; }
                sendMessage(xbus::MessageId::HardwareVersion,
                            std::array<std::uint8_t, 2> { 0x02, 0x01 });
                return;

            case static_cast<std::uint8_t>(xbus::MessageId::OutputConfiguration):
                handleOutputConfiguration(message);
                return;

            default:
                return;
        }
    }

    void handleOutputConfiguration(const xbus::MessageView& message)
    {
        // Config-state only, and the LLCP says so for both the request and the
        // set. A device that answered this while measuring would let a bug in
        // the state machine pass unnoticed.
        if (mMeasuring.load())
        {
            return;
        }

        // The Req/Set distinction is the payload length and nothing else.
        if (!message.data.empty())
        {
            if (mBehaviour.refuseWrite)
            {
                sendMessage(xbus::MessageId::ErrorReport, std::array<std::uint8_t, 1> { 0x21 });
                return;
            }

            const xbus::Result<std::size_t> count = xbus::output_entry_count(message.data);
            if (!count)
            {
                sendMessage(xbus::MessageId::ErrorReport, std::array<std::uint8_t, 1> { 0x04 });
                return;
            }

            std::vector<xbus::OutputEntry> accepted;
            for (std::size_t i = 0; i < *count; ++i)
            {
                const xbus::Result<xbus::OutputEntry> entry =
                    xbus::parse_output_entry(message.data, i);
                if (!entry) { break; }

                xbus::OutputEntry stored = *entry;

                // The device forces the maximum on anything that accompanies
                // every packet, whatever it was asked for.
                if (xbus::frequency_is_ignored(stored.rawId))
                {
                    stored.frequencyHz = xbus::kMaxFrequency;
                }
                else if (mBehaviour.clampHz != 0 && stored.frequencyHz > mBehaviour.clampHz &&
                         stored.frequencyHz != xbus::kMaxFrequency)
                {
                    // Clamped, not refused -- which is what makes reading the
                    // echo back the only way to know what the device will send.
                    stored.frequencyHz = mBehaviour.clampHz;
                }

                accepted.push_back(stored);
            }

            {
                const std::lock_guard<std::mutex> lock(mMutex);
                mBehaviour.outputs = accepted;
                mWritten = accepted;
            }
        }

        // Both the request and the acknowledgement of a set carry the current
        // list.
        std::vector<xbus::OutputEntry> current;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            current = mBehaviour.outputs;
        }

        std::vector<std::uint8_t> payload(current.size() * 4);
        const xbus::Result<std::size_t> used = xbus::encode_output_config(current, payload);
        if (!used) { return; }

        std::vector<std::uint8_t> out(*used + 5);
        const xbus::Result<std::size_t> size = xbus::encode_message(
            xbus::MessageId::OutputConfigurationAck,
            std::span<const std::uint8_t>(payload.data(), *used), out);
        if (size)
        {
            send(std::span<const std::uint8_t>(out.data(), *size));
        }
    }

    void run()
    {
        xbus::Framer framer;
        std::array<std::uint8_t, 512> buffer {};

        while (mRunning.load())
        {
            // poll() before read(), for the same reason SerialStream does it:
            // on a tty with VMIN=0 and VTIME=0 a read with nothing available
            // returns 0, which is indistinguishable from end of file unless
            // poll has already said there is something to read. Treating that
            // 0 as EOF made this fake device exit before answering anything.
            struct pollfd pfd { mDeviceFd, POLLIN, 0 };
            const int ready = ::poll(&pfd, 1, 5);

            if (ready > 0)
            {
                const ssize_t n = ::read(mDeviceFd, buffer.data(), buffer.size());

                if (n > 0)
                {
                    framer.push(std::span<const std::uint8_t>(buffer.data(),
                                                              static_cast<std::size_t>(n)));
                    while (const std::optional<xbus::MessageView> message = framer.next())
                    {
                        handle(*message);
                    }
                }
                else if (n == 0)
                {
                    break;
                }
                else if (errno != EAGAIN && errno != EINTR)
                {
                    break;
                }
            }
            else if (ready < 0 && errno != EINTR)
            {
                break;
            }

            if (mMeasuring.load() && mDataSent < mBehaviour.dataMessages)
            {
                sendData();
                ++mDataSent;

                if (mBehaviour.resetAfterData != 0 && mDataSent == mBehaviour.resetAfterData)
                {
                    // A brown-out. The device forgets it was measuring and
                    // announces itself again.
                    mMeasuring.store(false);
                    sendAck(xbus::MessageId::WakeUp);
                }
            }

            std::this_thread::sleep_for(1ms);
        }
    }

    Behaviour mBehaviour;
    mutable std::mutex mMutex;
    std::vector<xbus::OutputEntry> mWritten;

    int mHostFd { -1 };
    int mDeviceFd { -1 };
    std::thread mThread;
    std::atomic<bool> mRunning { true };
    std::atomic<bool> mMeasuring { false };
    std::atomic<unsigned> mGoToConfig { 0 };
    std::atomic<unsigned> mGoToMeasurement { 0 };
    unsigned mDataSent { 0 };
    unsigned mCounter { 1 };
};

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testSerialStreamRejectsBadInput()
{
    // A baud rate with no termios constant is refused rather than approximated.
    // A port opened at a rate the driver rounded receives nothing but checksum
    // errors, which reads as a broken device.
    const mti610::Result<std::unique_ptr<mti610::SerialStream>> odd =
        mti610::SerialStream::open("/dev/null", { .baud = 123456, .openTimeoutMs = 100 });
    check(!odd.has_value(), "an unsupported baud rate is refused");
    check(!odd && odd.error().kind == mti610::Error::Kind::InvalidArgument, "as invalid argument");

    check(mti610::is_supported_baud(115200), "115200 is supported");
    // 921600 has no termios constant on macOS and is applied by ioctl
    // instead. Asserted unconditionally so that a platform where neither works
    // fails here rather than opening a port at the wrong rate.
    check(mti610::is_supported_baud(921600), "and so is 921600, on both platforms");
    check(!mti610::is_supported_baud(123456), "123456 is not");

    // A path that does not exist.
    const mti610::Result<std::unique_ptr<mti610::SerialStream>> missing =
        mti610::SerialStream::open("/dev/definitely-not-a-serial-port",
                                   { .baud = 115200, .openTimeoutMs = 100 });
    check(!missing.has_value(), "a missing device node is refused");
    check(!missing && missing.error().kind == mti610::Error::Kind::NotFound, "as not found");

    // A regular file opens but is not a tty. Caught, because otherwise a path
    // typo that landed on a file would present as a device that never speaks.
    const mti610::Result<std::unique_ptr<mti610::SerialStream>> notATty =
        mti610::SerialStream::open("/dev/null", { .baud = 115200, .openTimeoutMs = 100 });
    check(!notATty.has_value(), "a non-tty is refused rather than read from");
}

void testHandshakeAgainstAScriptedDevice()
{
    FakeMti::Behaviour behaviour = FakeMti::defaults();
    behaviour.outputs = { { 0x4022, 50 } };
    behaviour.dataMessages = 200;
    FakeMti device(behaviour);
    check(device.valid(), "the pty opened");
    if (!device.valid()) { return; }

    std::atomic<int> samples { 0 };
    std::atomic<int> accelerationsSeen { 0 };
    std::atomic<double> lastZ { 0.0 };

    mti610::StreamClient::Options options;
    options.desiredOutputs = { { 0x1020, xbus::kMaxFrequency },
                               { 0x1060, xbus::kMaxFrequency },
                               { 0xE020, xbus::kMaxFrequency },
                               { 0x4022, 100 } };
    options.stopWhenStreamEnds = true;
    options.session.replyTimeoutMs = 500;

    const int fd = device.hostFd();
    mti610::StreamClient client(
        [fd]() -> mti610::Result<std::unique_ptr<mti610::ByteStream>> {
            return mti610::adopt_fd(::dup(fd), "pty");
        },
        options,
        [&](const xbus::MessageView& message) {
            ++samples;
            xbus::ItemIterator walk(message.data);
            while (const std::optional<xbus::DataItem> item = walk.next())
            {
                if (item->is(xbus::DataId::Acceleration))
                {
                    if (const xbus::Result<xbus::Acceleration> a =
                            xbus::Acceleration::parse(*item))
                    {
                        ++accelerationsSeen;
                        lastZ.store(a->zMps2);
                    }
                }
            }
        });

    client.start();

    check(waitFor([&] { return client.measuring(); }, 3000ms),
          "the handshake completes and the device enters measurement");

    check(waitFor([&] { return samples.load() >= 10; }, 3000ms),
          "and data messages arrive");

    // The whole path, end to end: a device encoded 9.81 as fp16.32, it went
    // through a real tty, and it came back out as 9.81.
    check(std::abs(lastZ.load() - 9.81) < 1e-6,
          "an fp16.32 acceleration survives the round trip through a real port");

    // The device's stored configuration was wrong and got corrected.
    check(device.goToConfigCount() >= 1, "the node entered config state");
    check(device.goToMeasurementCount() >= 1, "and left it again");

    const std::vector<xbus::OutputEntry> written = device.written();
    check(!written.empty(), "the output configuration was written");

    const mti610::DeviceInfo info = client.deviceInfo();
    check(info.deviceId == 0x03E81234ull, "the eight-byte device id was read correctly");
    check(info.productCode == "MTi-610R-2A5G4", "and the product code");
    check(info.looksLikeMti610(), "which is recognised as the device this library models");
    check(info.firmwareVersion() == "1.10.3.1234", "and the firmware version");

    const mti610::StreamClient::Stats stats = client.stats();
    check(stats.dataMessages >= 10, "the stats count the data messages");
    check(stats.items >= 40, "and their items");
    check(stats.malformedItems == 0, "none of which was malformed");
    check(stats.framer.checksumErrors == 0, "and nothing failed its checksum");

    client.stop();
}

void testDeviceThatNeverAnswersTimesOutRatherThanHanging()
{
    // What the wrong baud rate looks like: the bytes go out, nothing comes
    // back. It has to present as a bounded failure, not as a thread parked
    // forever in a read.
    FakeMti::Behaviour behaviour = FakeMti::defaults();
    behaviour.answerGoToConfig = false;
    behaviour.dataMessages = 0;
    FakeMti device(behaviour);
    if (!device.valid()) { return; }

    mti610::StreamClient::Options options;
    options.stopWhenStreamEnds = true;
    options.session.replyTimeoutMs = 100;
    options.session.retries = 1;

    const int fd = device.hostFd();
    mti610::StreamClient client(
        [fd]() -> mti610::Result<std::unique_ptr<mti610::ByteStream>> {
            return mti610::adopt_fd(::dup(fd), "pty");
        },
        options, [](const xbus::MessageView&) {});

    const auto started = std::chrono::steady_clock::now();
    client.start();

    check(waitFor([&] { return !client.running(); }, 5000ms),
          "a device that never answers stops rather than hanging");

    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(elapsed < 5s, "and does so within the timeout budget, not eventually");
    check(!client.measuring(), "and never reports that it is measuring");

    client.stop();
}

void testDeviceResetIsNoticedAndRecovered()
{
    // A brown-out. The device forgets the configuration we wrote and announces
    // itself with a WakeUp. Missing this is the failure that does not
    // announce itself: the node keeps publishing, with the device's STORED
    // outputs at the device's stored rates, and nothing anywhere says so.
    FakeMti::Behaviour behaviour = FakeMti::defaults();
    behaviour.outputs = { { 0x4022, 50 } };
    behaviour.resetAfterData = 5;
    behaviour.dataMessages = 200;
    FakeMti device(behaviour);
    if (!device.valid()) { return; }

    mti610::StreamClient::Options options;
    options.desiredOutputs = { { 0x4022, 100 } };
    options.stopWhenStreamEnds = true;
    options.session.replyTimeoutMs = 500;

    const int fd = device.hostFd();
    mti610::StreamClient client(
        [fd]() -> mti610::Result<std::unique_ptr<mti610::ByteStream>> {
            return mti610::adopt_fd(::dup(fd), "pty");
        },
        options, [](const xbus::MessageView&) {});

    client.start();

    check(waitFor([&] { return client.measuring(); }, 3000ms), "the first handshake completes");

    const std::uint64_t before = client.configGeneration();

    check(waitFor([&] { return client.stats().deviceResets >= 1; }, 3000ms),
          "the reset is noticed");

    check(waitFor([&] { return client.configGeneration() > before; }, 3000ms),
          "and the configuration is re-applied rather than assumed to have survived");

    check(waitFor([&] { return client.measuring(); }, 3000ms),
          "and the device is put back into measurement");

    check(device.goToConfigCount() >= 2, "which took a second trip through config state");

    client.stop();
}

void testRefusedWriteIsReportedNotRetriedForever()
{
    // The device understood the message and declined it. Sending the same
    // bytes again gets the same answer, so a retry only delays the report.
    FakeMti::Behaviour behaviour = FakeMti::defaults();
    behaviour.outputs = { { 0x4022, 50 } };
    behaviour.refuseWrite = true;
    behaviour.dataMessages = 0;
    FakeMti device(behaviour);
    if (!device.valid()) { return; }

    const int fd = ::dup(device.hostFd());
    std::unique_ptr<mti610::ByteStream> stream = mti610::adopt_fd(fd, "pty");

    xbus::Framer framer;
    mti610::DeviceSession session(*stream, framer,
                                  { .replyTimeoutMs = 500,
                                    .retries = 2,
                                    .mode = mti610::ConfigMode::Enforce,
                                    .policy = mti610::PortPolicy::Additive });

    check(session.goToConfig().has_value(), "config state is entered");

    const mti610::Result<mti610::ConfigureResult> configured =
        session.configure({ { 0x4022, 100 } });

    check(!configured.has_value(), "a refused write fails");
    check(!configured && configured.error().kind == mti610::Error::Kind::Refused,
          "as a refusal rather than as a timeout");
    check(!configured && configured.error().message.find("parameter") != std::string::npos,
          "carrying the device's own reason");
}

void testDeviceClampsRatherThanRefuses()
{
    // A device whose link cannot carry the configuration lowers the rate and
    // says nothing. Reading the echo back is the only way to know -- assuming
    // the write took would make the node's status message a lie.
    FakeMti::Behaviour behaviour = FakeMti::defaults();
    behaviour.clampHz = 40;
    behaviour.dataMessages = 0;
    FakeMti device(behaviour);
    if (!device.valid()) { return; }

    const int fd = ::dup(device.hostFd());
    std::unique_ptr<mti610::ByteStream> stream = mti610::adopt_fd(fd, "pty");

    xbus::Framer framer;
    mti610::DeviceSession session(*stream, framer,
                                  { .replyTimeoutMs = 500,
                                    .retries = 2,
                                    .mode = mti610::ConfigMode::Enforce,
                                    .policy = mti610::PortPolicy::Exclusive });

    check(session.goToConfig().has_value(), "config state is entered");

    const mti610::Result<mti610::ConfigureResult> configured =
        session.configure({ { 0x4022, 100 }, { 0x1020, 100 } });

    check(configured.has_value(), "the write succeeds");
    if (!configured) { return; }

    check(configured->wrote, "and reports that it wrote");

    std::uint16_t accelHz = 0;
    std::uint16_t counterHz = 0;
    for (const xbus::OutputEntry& entry : configured->effective)
    {
        if (entry.rawId == 0x4022) { accelHz = entry.frequencyHz; }
        if (entry.rawId == 0x1020) { counterHz = entry.frequencyHz; }
    }

    check(accelHz == 40, "the effective rate is what the device settled on, not what was asked");
    check(counterHz == xbus::kMaxFrequency,
          "and packet metadata comes back at the maximum whatever was asked");
}

void testReplayStream()
{
    // No device at all: a capture through the same code path.
    std::vector<std::uint8_t> capture;
    for (int i = 0; i < 5; ++i)
    {
        const std::array<std::uint8_t, 10> message {
            0xFA, 0xFF, 0x36, 0x05, 0x10, 0x20, 0x02, 0x04, 0xD2, 0xBE
        };
        capture.insert(capture.end(), message.begin(), message.end());
    }

    std::atomic<int> seen { 0 };

    mti610::StreamClient::Options options;
    options.readOnly = true;  // nothing to configure in a capture
    options.stopWhenStreamEnds = true;

    auto bytes = capture;
    mti610::StreamClient client(
        [bytes]() -> mti610::Result<std::unique_ptr<mti610::ByteStream>> {
            return mti610::ReplayStream::fromBytes(
                bytes, { .chunkSize = 3, .loop = false, .chunkDelayMs = 0 });
        },
        options, [&](const xbus::MessageView&) { ++seen; });

    client.start();
    check(waitFor([&] { return !client.running(); }, 3000ms), "a replay runs to the end and stops");
    check(seen.load() == 5, "delivering every message, three bytes at a time");
    client.stop();
}

void testReplayRejectsAnEmptyCapture()
{
    // An empty file would otherwise present as an immediate clean end of
    // stream, which looks exactly like a device that came up and said nothing.
    const char* tmp = "/tmp/mti610_empty_capture.bin";
    { std::ofstream(tmp).close(); }

    const mti610::Result<std::unique_ptr<mti610::ReplayStream>> replay =
        mti610::ReplayStream::open(tmp, {});
    check(!replay.has_value(), "an empty capture is refused");
    ::unlink(tmp);

    const mti610::Result<std::unique_ptr<mti610::ReplayStream>> missing =
        mti610::ReplayStream::open("/tmp/mti610-no-such-capture.bin", {});
    check(!missing.has_value(), "and so is a missing one");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    testSerialStreamRejectsBadInput();
    testHandshakeAgainstAScriptedDevice();
    testDeviceThatNeverAnswersTimesOutRatherThanHanging();
    testDeviceResetIsNoticedAndRecovered();
    testRefusedWriteIsReportedNotRetriedForever();
    testDeviceClampsRatherThanRefuses();
    testReplayStream();
    testReplayRejectsAnEmptyCapture();

    return failures == 0 ? 0 : 1;
}
