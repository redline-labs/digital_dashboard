// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transport half, against a scripted receiver on the loopback interface.
//
// No hardware and no network beyond 127.0.0.1, but real sockets: the point is
// to exercise the parts that a byte-level test cannot reach -- that a stream
// arriving in TCP-sized fragments still yields whole records, that a receiver
// disappearing mid-transmission is recovered from rather than wedging the
// reader thread, and that the control exchange picks its reply out of a stream
// that also carries GSOF.
//
// The MockReceiver pattern is lifted from libs/apple_usb/test_usbmux_client.cpp:
// bind in the constructor so the port is known before anything connects, serve
// on a background thread, and tear down in the destructor.

#include "bd992/control_client.h"
#include "bd992/output_config.h"
#include "bd992/replay_stream.h"
#include "bd992/stream_client.h"
#include "bd992/tcp_stream.h"
#include "gsof/commands.h"
#include "gsof/records.h"
#include "gsof/trimcomm.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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

using namespace bd992;
using namespace std::chrono_literals;

// ============================================================================
// Building a plausible GSOF stream
// ============================================================================

// One GENOUT packet carrying `records` as a single-page transmission.
std::vector<std::uint8_t> genout(std::uint8_t txNumber, const std::vector<std::uint8_t>& records)
{
    std::vector<std::uint8_t> data { txNumber, 0, 0 };
    data.insert(data.end(), records.begin(), records.end());

    std::vector<std::uint8_t> packet(data.size() + gsof::trimcomm::kOverheadSize);
    const gsof::Result<std::size_t> written =
        gsof::trimcomm::encode_packet(gsof::trimcomm::PacketType::GenOut, data, packet);
    check(written.has_value(), "the test's own GENOUT packet encodes");
    return packet;
}

// A TLV record around a body.
std::vector<std::uint8_t> record(std::uint8_t type, const std::vector<std::uint8_t>& body)
{
    std::vector<std::uint8_t> out { type, static_cast<std::uint8_t>(body.size()) };
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Position time (record 1), with a recognisable week number so a test can tell
// one epoch from another.
std::vector<std::uint8_t> positionTimeRecord(std::uint16_t week)
{
    return record(1, { 0x00, 0x01, 0x02, 0x03, static_cast<std::uint8_t>(week >> 8),
                       static_cast<std::uint8_t>(week & 0xFF), 25, 0xBF, 0x00, 0x00 });
}

// A stream of `count` transmissions, each holding one position time record.
std::vector<std::uint8_t> gsofStream(std::uint16_t count)
{
    std::vector<std::uint8_t> stream;
    for (std::uint16_t i = 0; i < count; ++i)
    {
        const std::vector<std::uint8_t> packet = genout(static_cast<std::uint8_t>(i), positionTimeRecord(i));
        stream.insert(stream.end(), packet.begin(), packet.end());
    }
    return stream;
}

// ============================================================================
// A receiver that is not there
// ============================================================================

class MockReceiver
{
  public:
    // Called with each client socket. Return when done with that connection.
    using Serve = std::function<void(int)>;

    explicit MockReceiver(Serve serve) : mServe(std::move(serve))
    {
        mListen = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(mListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        // htonl/ntohs are macros in the macOS SDK, so they cannot be
        // qualified with :: the way the socket functions are.
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // let the kernel pick, so parallel tests cannot collide

        if (::bind(mListen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(mListen, 4) != 0)
        {
            ::close(mListen);
            mListen = -1;
            return;
        }

        sockaddr_in bound {};
        socklen_t length = sizeof(bound);
        ::getsockname(mListen, reinterpret_cast<sockaddr*>(&bound), &length);
        mPort = ntohs(bound.sin_port);

        mThread = std::thread([this] { accept(); });
    }

    ~MockReceiver()
    {
        mStopping.store(true);
        if (mListen >= 0)
        {
            ::shutdown(mListen, SHUT_RDWR);
            ::close(mListen);
            mListen = -1;
        }
        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    MockReceiver(const MockReceiver&) = delete;
    MockReceiver& operator=(const MockReceiver&) = delete;

    std::uint16_t port() const { return mPort; }
    bool ok() const { return mPort != 0; }
    int connections() const { return mConnections.load(); }

  private:
    void accept()
    {
        while (!mStopping.load())
        {
            pollfd pfd { mListen, POLLIN, 0 };
            // A short tick so teardown is prompt rather than waiting on a
            // connection that will never come.
            if (::poll(&pfd, 1, 100) <= 0)
            {
                continue;
            }

            const int client = ::accept(mListen, nullptr, nullptr);
            if (client < 0)
            {
                continue;
            }

            ++mConnections;
            mServe(client);
            ::close(client);
        }
    }

    Serve mServe;
    int mListen { -1 };
    std::uint16_t mPort { 0 };
    std::thread mThread;
    std::atomic<bool> mStopping { false };
    std::atomic<int> mConnections { 0 };
};

bool sendAll(int fd, const std::vector<std::uint8_t>& bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Wait for a predicate, up to a deadline. Returns whether it came true --
// never sleeps for a fixed duration and hopes, because a test that does that
// is either slow or flaky and usually both.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds limit = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

StreamClient::StreamFactory tcpFactory(std::uint16_t port)
{
    return [port]() -> Result<std::unique_ptr<ByteStream>> {
        Result<std::unique_ptr<TcpStream>> stream = TcpStream::connect("127.0.0.1", port, 1000ms);
        if (!stream.has_value())
        {
            return std::unexpected(stream.error());
        }
        return std::unique_ptr<ByteStream>(std::move(*stream));
    };
}

// ============================================================================
// The stream path
// ============================================================================

void test_records_arrive_over_a_real_socket()
{
    const std::vector<std::uint8_t> stream = gsofStream(20);

    MockReceiver receiver([&stream](int client) {
        // Deliberately dribbled out in small pieces. A receiver does not send
        // packet-aligned writes and neither does TCP.
        for (std::size_t at = 0; at < stream.size(); at += 7)
        {
            const std::size_t take = std::min<std::size_t>(7, stream.size() - at);
            if (::send(client, stream.data() + at, take, MSG_NOSIGNAL) <= 0)
            {
                return;
            }
        }
        // Hold the connection open so the client does not see an immediate
        // close and reconnect.
        std::this_thread::sleep_for(300ms);
    });

    check(receiver.ok(), "the mock receiver bound a port");
    if (!receiver.ok())
    {
        return;
    }

    std::mutex mutex;
    std::vector<std::uint16_t> weeks;

    StreamClient client(tcpFactory(receiver.port()), StreamClient::Options {},
                        [&mutex, &weeks](const gsof::RawRecord& raw) {
                            const gsof::Result<gsof::PositionTime> parsed = gsof::PositionTime::parse(raw.body);
                            if (parsed.has_value())
                            {
                                const std::lock_guard<std::mutex> lock(mutex);
                                weeks.push_back(parsed->gpsWeek);
                            }
                        });

    client.start();

    const bool got = waitFor([&mutex, &weeks] {
        const std::lock_guard<std::mutex> lock(mutex);
        return weeks.size() >= 20;
    });

    client.stop();

    check(got, "all twenty transmissions arrive over a socket that fragments them");

    const std::lock_guard<std::mutex> lock(mutex);
    bool inOrder = true;
    for (std::size_t i = 0; i < weeks.size() && i < 20; ++i)
    {
        inOrder = inOrder && weeks[i] == i;
    }
    check(inOrder, "and in order, with their contents intact");

    const StreamClient::Stats stats = client.stats();
    check(stats.records >= 20, "the record count matches what was delivered");
    check(stats.framer.checksumErrors == 0, "no checksum errors on a clean stream");
    check(stats.framer.resyncs == 0, "and no resynchronisation");
}

void test_a_dropped_connection_is_reconnected()
{
    // THE case a vehicle produces: the receiver loses power with the ignition
    // and comes back. A node that needed restarting would be useless.
    std::atomic<int> served { 0 };

    MockReceiver receiver([&served](int client) {
        const int attempt = served.fetch_add(1);

        if (attempt == 0)
        {
            // Half a transmission, then hang up mid-packet.
            const std::vector<std::uint8_t> packet = genout(0, positionTimeRecord(1000));
            const std::vector<std::uint8_t> half(packet.begin(), packet.begin() + 6);
            sendAll(client, half);
            return;
        }

        // Second time around, behave.
        for (std::uint16_t i = 0; i < 5; ++i)
        {
            if (!sendAll(client, genout(static_cast<std::uint8_t>(i), positionTimeRecord(i))))
            {
                return;
            }
        }
        std::this_thread::sleep_for(500ms);
    });

    check(receiver.ok(), "the mock receiver bound a port");
    if (!receiver.ok())
    {
        return;
    }

    std::atomic<int> records { 0 };

    StreamClient::Options options;
    options.reconnectBackoff = { 50ms };

    StreamClient client(tcpFactory(receiver.port()), options,
                        [&records](const gsof::RawRecord&) { ++records; });

    client.start();

    const bool recovered = waitFor([&records] { return records.load() >= 5; });

    client.stop();

    check(recovered, "records arrive after the connection was dropped mid-packet");
    check(served.load() >= 2, "which required reconnecting");

    const StreamClient::Stats stats = client.stats();
    check(stats.connects >= 2, "the reconnection is counted");
    check(stats.disconnects >= 1, "and so is the drop");

    // The truncated packet from the first connection must not have combined
    // with bytes from the second: five records went out after the reconnect
    // and five is what should arrive, with no sixth assembled from the
    // fragment.
    check(records.load() == 5, "the fragment from before the drop did not become a record");
    check(stats.framer.checksumErrors == 0,
          "nor did it corrupt the first packet of the new connection");
}

void test_a_receiver_that_is_not_listening_is_retried_not_fatal()
{
    // Nothing bound on this port. The client must keep trying rather than
    // exiting, because the receiver may simply not be powered up yet.
    StreamClient::Options options;
    options.reconnectBackoff = { 20ms };

    StreamClient client(tcpFactory(1), options, [](const gsof::RawRecord&) {});
    client.start();

    const bool retried = waitFor([&client] { return client.stats().connectFailures >= 3; }, 3000ms);

    check(retried, "a refused connection is retried");
    check(client.isRunning(), "and the reader thread stays alive");

    const StreamClient::Stats stats = client.stats();
    check(!stats.connected, "while reporting itself as disconnected");
    check(!stats.lastError.empty(), "with the reason available for the status message");

    client.stop();
    check(!client.isRunning(), "and stop() ends it promptly");
}

// ============================================================================
// Replay -- the same pipeline, no socket
// ============================================================================

void test_replay_drives_the_same_pipeline()
{
    const std::vector<std::uint8_t> stream = gsofStream(12);

    std::atomic<int> records { 0 };
    std::atomic<int> transmissions { 0 };

    StreamClient::Options options;
    options.stopWhenStreamEnds = true;

    ReplayStream::Options replayOptions;
    replayOptions.chunkSize = 5;

    StreamClient client(
        [&stream, replayOptions]() -> Result<std::unique_ptr<ByteStream>> {
            return std::unique_ptr<ByteStream>(ReplayStream::fromBytes(stream, replayOptions));
        },
        options, [&records](const gsof::RawRecord&) { ++records; });

    client.setTransmissionHandler([&transmissions] { ++transmissions; });

    client.start();
    const bool finished = waitFor([&client] { return !client.isRunning(); });
    client.stop();

    check(finished, "a replay stream ends on its own rather than reconnecting forever");
    check(records.load() == 12, "and every record in the capture is decoded");
    check(transmissions.load() == 12, "one transmission handler call per transmission");
}

void test_replay_loops_when_asked()
{
    const std::vector<std::uint8_t> stream = gsofStream(3);

    std::atomic<int> records { 0 };

    ReplayStream::Options replayOptions;
    replayOptions.loop = true;
    replayOptions.chunkSize = 16;

    StreamClient client(
        [&stream, replayOptions]() -> Result<std::unique_ptr<ByteStream>> {
            return std::unique_ptr<ByteStream>(ReplayStream::fromBytes(stream, replayOptions));
        },
        StreamClient::Options {}, [&records](const gsof::RawRecord&) { ++records; });

    client.start();
    const bool looped = waitFor([&records] { return records.load() > 9; });
    client.stop();

    check(looped, "a looping replay keeps producing records past the end of the capture");
}

// ============================================================================
// The control path
// ============================================================================

// The APPFILE reply a receiver would send describing two GSOF outputs.
std::vector<std::uint8_t> applicationFileReply(std::uint8_t deviceType)
{
    std::vector<std::uint8_t> records;
    const auto append = [&records](std::span<const std::uint8_t> bytes) {
        records.insert(records.end(), bytes.begin(), bytes.end());
    };

    append(gsof::appfile::gsof_output_record(gsof::appfile::PortIndex::IpSocket1,
                                             gsof::appfile::Frequency::Hz10,
                                             gsof::RecordType::LatLongHeight));
    append(gsof::appfile::gsof_output_record(gsof::appfile::PortIndex::IpSocket1,
                                             gsof::appfile::Frequency::Hz1,
                                             gsof::RecordType::Velocity));

    std::array<std::uint8_t, gsof::trimcomm::kMaxPacketSize> packet {};
    gsof::appfile::FileControl control {};
    control.deviceType = deviceType;

    const gsof::Result<std::size_t> written =
        gsof::appfile::encode_application_file(control, records, 0x11, packet);
    check(written.has_value(), "the test's own APPFILE reply encodes");

    return std::vector<std::uint8_t>(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(*written));
}

ControlClient::StreamFactory controlFactory(std::uint16_t port)
{
    return [port]() -> Result<std::unique_ptr<ByteStream>> {
        Result<std::unique_ptr<TcpStream>> stream = TcpStream::connect("127.0.0.1", port, 1000ms);
        if (!stream.has_value())
        {
            return std::unexpected(stream.error());
        }
        return std::unique_ptr<ByteStream>(std::move(*stream));
    };
}

void test_reading_a_configuration_back()
{
    MockReceiver receiver([](int client) {
        std::uint8_t request[64] = {};
        const ssize_t n = ::recv(client, request, sizeof(request), 0);
        if (n <= 0)
        {
            return;
        }

        // Answer only a GETAPPFILE, and only after some GSOF noise -- a real
        // port may be carrying both, and the reply has to be picked out of it.
        if (request[2] != 0x65)
        {
            return;
        }

        sendAll(client, genout(0, positionTimeRecord(2000)));
        sendAll(client, applicationFileReply(0x7B));
        std::this_thread::sleep_for(200ms);
    });

    check(receiver.ok(), "the mock receiver bound a port");
    if (!receiver.ok())
    {
        return;
    }

    ControlClient::Options options;
    options.replyTimeout = 2000ms;

    ControlClient control(controlFactory(receiver.port()), options);

    const Result<gsof::appfile::ApplicationFile> file = control.readApplicationFile(1);

    check(file.has_value(), "the application file is read back");
    if (!file.has_value())
    {
        SPDLOG_ERROR("  reason: {}", to_string(file.error()));
        return;
    }

    check(file->outputCount == 2, "both output messages are present");
    check(file->control.deviceType == 0x7B, "and the device type the receiver reported");
    check(control.deviceType() == 0x7B,
          "which is remembered, so a subsequent write echoes it rather than guessing");

    // And the whole point: it can be diffed.
    const std::vector<OutputMessage> desired {
        gsof_output(PortIndex::IpSocket1, gsof::RecordType::LatLongHeight, Frequency::Hz10),
        gsof_output(PortIndex::IpSocket1, gsof::RecordType::Velocity, Frequency::Hz10),
    };

    const std::vector<Change> changes = diff(file->view(), desired, PortIndex::IpSocket1, PortPolicy::Additive);
    check(changes.size() == 1 && changes[0].kind == ChangeKind::RateDrift,
          "and diffed against the configuration: velocity is at 1 Hz, not 10");
}

void test_a_nak_is_reported_as_a_refusal()
{
    MockReceiver receiver([](int client) {
        std::uint8_t request[64] = {};
        if (::recv(client, request, sizeof(request), 0) <= 0)
        {
            return;
        }

        // NAK carries no data.
        std::array<std::uint8_t, gsof::trimcomm::kOverheadSize> nak {};
        const gsof::Result<std::size_t> written = gsof::trimcomm::encode_packet(
            gsof::trimcomm::PacketType::Nak, std::span<const std::uint8_t>(), nak);
        if (written.has_value())
        {
            sendAll(client, std::vector<std::uint8_t>(nak.begin(), nak.end()));
        }
        std::this_thread::sleep_for(200ms);
    });

    check(receiver.ok(), "the mock receiver bound a port");
    if (!receiver.ok())
    {
        return;
    }

    ControlClient::Options options;
    options.replyTimeout = 2000ms;

    ControlClient control(controlFactory(receiver.port()), options);

    const Result<gsof::appfile::ApplicationFile> file = control.readApplicationFile(9);

    check(!file.has_value(), "a NAK is not success");
    if (!file.has_value())
    {
        check(file.error().kind == Error::Kind::Refused,
              "and is reported as a refusal, not a timeout -- retrying would get the same answer");
    }
}

void test_a_silent_receiver_times_out()
{
    MockReceiver receiver([](int client) {
        std::uint8_t request[64] = {};
        ::recv(client, request, sizeof(request), 0);
        // Say nothing at all.
        std::this_thread::sleep_for(500ms);
    });

    check(receiver.ok(), "the mock receiver bound a port");
    if (!receiver.ok())
    {
        return;
    }

    ControlClient::Options options;
    options.replyTimeout = 250ms;

    ControlClient control(controlFactory(receiver.port()), options);

    const auto started = std::chrono::steady_clock::now();
    const Result<gsof::appfile::ApplicationFile> file = control.readApplicationFile(1);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(!file.has_value() && file.error().kind == Error::Kind::Timeout,
          "a receiver that says nothing is a timeout");
    check(elapsed < 3000ms, "and the deadline is honoured rather than waiting forever");
}

void test_raw_commands_are_gated()
{
    ControlClient::Options options;
    options.allowRawCommands = false;

    ControlClient control(
        []() -> Result<std::unique_ptr<ByteStream>> { return not_connected("no receiver in this test"); },
        options);

    const Result<ControlClient::Reply> reply = control.sendRaw(0x6F, {});

    check(!reply.has_value() && reply.error().kind == Error::Kind::NotPermitted,
          "a raw command is refused locally when not enabled -- it never reaches the socket");
}

void test_an_empty_write_is_refused()
{
    ControlClient control(
        []() -> Result<std::unique_ptr<ByteStream>> { return not_connected("no receiver in this test"); },
        ControlClient::Options {});

    const Result<void> written = control.writeApplicationFile({});

    check(!written.has_value() && written.error().kind == Error::Kind::InvalidArgument,
          "an application file with no records is refused before anything is sent");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    test_records_arrive_over_a_real_socket();
    test_a_dropped_connection_is_reconnected();
    test_a_receiver_that_is_not_listening_is_retried_not_fatal();
    test_replay_drives_the_same_pipeline();
    test_replay_loops_when_asked();
    test_reading_a_configuration_back();
    test_a_nak_is_reported_as_a_refusal();
    test_a_silent_receiver_times_out();
    test_raw_commands_are_gated();
    test_an_empty_write_is_refused();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    spdlog::set_level(spdlog::level::info);
    SPDLOG_INFO("all bd992 stream and control checks passed");
    return 0;
}
