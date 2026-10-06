// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RTSP receiver, driven over loopback the way a phone drives it.
//
// Everything here runs before pair-verify, so the control channel is plaintext
// and no phone, MFi part or pairing is needed. That covers what the receiver
// decides on its own: how requests are routed, and what it owns per stream.
#include "airplay/receiver.h"

#include "airplay/crypto.h"
#include "airplay/media_stream.h"

#include "airplay/rtsp.h"
#include "plist/binary.h"
#include "plist/value.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

struct Response
{
    int status = 0;
    std::string head;
    std::string body;
};

// One control connection to the receiver, as the phone holds it.
class Client
{
  public:
    explicit Client(uint16_t port)
    {
        fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr = in6addr_loopback;
        if (fd_ >= 0 && ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~Client()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connected() const { return fd_ >= 0; }

    // Sends one request and reads its response. nullopt on a closed socket or
    // a response that did not arrive within two seconds.
    std::optional<Response> request(const std::string& method, const std::string& uri,
                                     const std::string& body = {},
                                     const std::string& content_type = {})
    {
        std::string wire = method + " " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq_) +
                           "\r\n";
        if (!content_type.empty())
        {
            wire += "Content-Type: " + content_type + "\r\n";
        }
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        if (::send(fd_, wire.data(), wire.size(), 0) != static_cast<ssize_t>(wire.size()))
        {
            return std::nullopt;
        }
        return read();
    }

  private:
    std::optional<Response> read()
    {
        std::string buffer;
        size_t header_end = std::string::npos;
        size_t length = 0;
        while (true)
        {
            if (header_end == std::string::npos)
            {
                header_end = buffer.find("\r\n\r\n");
                if (header_end != std::string::npos)
                {
                    const size_t at = buffer.find("Content-Length: ");
                    if (at != std::string::npos && at < header_end)
                    {
                        const char* first = buffer.data() + at + 16;
                        std::from_chars(first, buffer.data() + header_end, length);
                    }
                }
            }
            if (header_end != std::string::npos && buffer.size() >= header_end + 4 + length)
            {
                break;
            }
            pollfd pfd{fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 2000) <= 0)
            {
                return std::nullopt;
            }
            char chunk[4096];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0)
            {
                return std::nullopt;
            }
            buffer.append(chunk, static_cast<size_t>(n));
        }
        Response response;
        response.head = buffer.substr(0, header_end);
        response.body = buffer.substr(header_end + 4, length);
        // "RTSP/1.0 200 OK"
        const size_t space = response.head.find(' ');
        if (space != std::string::npos)
        {
            std::from_chars(response.head.data() + space + 1,
                            response.head.data() + response.head.size(), response.status);
        }
        return response;
    }

    int fd_ = -1;
    int cseq_ = 0;
};

airplay::ReceiverConfig makeConfig(const std::filesystem::path& state_dir)
{
    airplay::ReceiverConfig config;
    config.bind_address = "::1";
    config.port = 0;
    config.width = 800;
    config.height = 480;
    config.fps = 30;
    config.name = "Test Receiver";
    config.model = "TestModel1,1";
    config.device_id = "02:00:00:00:00:09";
    config.state_dir = state_dir.string();
    return config;
}

void testRouting(uint16_t port)
{
    Client phone(port);
    expect(phone.connected(), "the phone connects to the control port");

    const auto info = phone.request("GET", "/info");
    expect(info && info->status == 200 && info->body.starts_with("bplist00"),
           "GET /info answers a binary plist");

    // The same request in absolute form routes the same way.
    const auto absolute =
        phone.request("GET", "rtsp://[::1]:" + std::to_string(port) + "/info?x=1");
    expect(absolute && absolute->status == 200 && absolute->body.starts_with("bplist00"),
           "an absolute URI with a query still reaches /info");

    // A request type nothing handles is acknowledged, not refused: a 501 makes
    // the phone treat the whole session as broken.
    const auto unknown = phone.request("POST", "/a-request-added-later", "abc",
                                       "application/octet-stream");
    expect(unknown && unknown->status == 200, "an unknown request is acknowledged with 200");
    const auto unknown_method = phone.request("ANNOUNCE", "rtsp://[::1]/1234");
    expect(unknown_method && unknown_method->status == 200,
           "an unhandled method is acknowledged with 200");

    // And the connection survives it.
    const auto after = phone.request("GET", "/info");
    expect(after && after->status == 200, "the connection is still served afterwards");
}

// Polls `done` for up to a second; the receiver reports from its own threads.
template <typename Predicate>
bool eventually(Predicate done)
{
    for (int i = 0; i < 100 && !done(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// A control connection that carried a session closing, with USB still there, is
// the phone ending CarPlay: the owner has to restart iAP2 to get it back. One
// that closes before RECORD is the phone still setting up, and one closed by
// our own stop() is not the phone's doing.
void testSessionLost(const std::filesystem::path& state_dir)
{
    airplay::Receiver receiver(makeConfig(state_dir));
    std::atomic<int> lost{0};
    receiver.setSessionLostHandler([&lost] { ++lost; });
    expect(receiver.start(), "a second receiver starts");

    {
        Client probe(receiver.port());
        expect(probe.request("GET", "/info").has_value(), "a probing connection is answered");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    expect(lost.load() == 0, "a connection that never reached RECORD is not a lost session");

    {
        Client phone(receiver.port());
        const auto record = phone.request("RECORD", "rtsp://[::1]/1234");
        expect(record && record->status == 200, "RECORD is answered");
    }
    expect(eventually([&] { return lost.load() == 1; }),
           "closing a live session's connection reports the session lost");

    Client again(receiver.port());
    const auto record = again.request("RECORD", "rtsp://[::1]/1234");
    expect(record && record->status == 200, "a new session reaches RECORD");
    receiver.stop();
    expect(lost.load() == 1, "stopping the receiver under a live session is not reported");
}

// Descriptors this process has open. Every stream is sockets, so a stream that
// outlives its TEARDOWN shows here.
int openFdCount()
{
    int count = 0;
    if (DIR* dir = ::opendir("/dev/fd"); dir != nullptr)
    {
        while (::readdir(dir) != nullptr)
        {
            ++count;
        }
        ::closedir(dir);
    }
    return count;
}

std::string encode(const plist::Value& value)
{
    const auto bytes = plist::encodeBinary(value);
    return std::string(bytes.begin(), bytes.end());
}

std::string streamsBody(std::initializer_list<int64_t> types)
{
    std::vector<plist::Value> streams;
    int64_t connection_id = 1;
    for (const int64_t type : types)
    {
        plist::Value entry = plist::Value::dict();
        entry.set("type", plist::Value::integer(type));
        entry.set("streamConnectionID", plist::Value::integer(connection_id++));
        if (type != 110)
        {
            entry.set("audioFormat", plist::Value::integer(0x800));
            entry.set("audioType", plist::Value::string(type == 101 ? "default" : "media"));
        }
        streams.push_back(std::move(entry));
    }
    plist::Value body = plist::Value::dict();
    body.set("streams", plist::Value::array(std::move(streams)));
    return encode(body);
}

const std::string kPlist = "application/x-apple-binary-plist";

bool setupStreams(Client& phone, std::initializer_list<int64_t> types)
{
    const auto reply = phone.request("SETUP", "rtsp://[::1]/1234", streamsBody(types), kPlist);
    return reply && reply->status == 200;
}

bool teardownStreams(Client& phone, std::initializer_list<int64_t> types)
{
    const auto reply = phone.request("TEARDOWN", "rtsp://[::1]/1234", streamsBody(types), kPlist);
    return reply && reply->status == 200;
}

// Every stream the phone sets up is a thread and its sockets. A TEARDOWN that
// only forgot about the stream left both behind, once for every prompt, track
// and call -- on a long drive, hundreds of threads, then SETUP failing when
// the descriptors ran out.
void testStreamLifetime(const std::filesystem::path& state_dir)
{
    airplay::Receiver receiver(makeConfig(state_dir));
    expect(receiver.start(), "the stream receiver starts");
    Client phone(receiver.port());
    plist::Value session = plist::Value::dict();
    session.set("name", plist::Value::string("Test Phone"));
    const auto session_reply = phone.request("SETUP", "rtsp://[::1]/1234", encode(session), kPlist);
    expect(session_reply && session_reply->status == 200, "the session SETUP is answered");

    const int baseline = openFdCount();
    expect(setupStreams(phone, {100, 110}), "audio and video SETUP is answered");
    expect(receiver.openStreamCount() == 2, "two streams are open");
    expect(openFdCount() == baseline + 3, "audio holds two sockets and video one");

    expect(teardownStreams(phone, {100}), "the audio TEARDOWN is answered");
    expect(receiver.openStreamCount() == 1, "tearing down audio closes only audio");
    expect(openFdCount() == baseline + 1, "and releases both of its sockets");

    expect(teardownStreams(phone, {110}), "the video TEARDOWN is answered");
    expect(receiver.openStreamCount() == 0 && openFdCount() == baseline,
           "tearing down video releases its listener too");

    // What a long drive does: a prompt, a track, a call, each a SETUP and a
    // TEARDOWN of the same stream.
    for (int i = 0; i < 25; ++i)
    {
        setupStreams(phone, {100, 101});
        teardownStreams(phone, {101});
        teardownStreams(phone, {100});
    }
    expect(receiver.openStreamCount() == 0, "25 audio cycles leave no stream open");
    expect(openFdCount() == baseline, "and no socket behind");

    // A SETUP for a type already open replaces it rather than adding a second.
    setupStreams(phone, {100});
    setupStreams(phone, {100});
    expect(receiver.openStreamCount() == 1 && openFdCount() == baseline + 2,
           "a repeated SETUP replaces the open stream");

    // A TEARDOWN with no stream list ends the session, and its streams.
    setupStreams(phone, {110});
    expect(phone.request("TEARDOWN", "rtsp://[::1]/1234").has_value(), "a full TEARDOWN is answered");
    expect(receiver.openStreamCount() == 0 && openFdCount() == baseline,
           "a full TEARDOWN closes every stream");

    // So does the control connection closing, without a TEARDOWN at all.
    setupStreams(phone, {100, 110});
    expect(receiver.openStreamCount() == 2, "streams are open again");
    {
        Client other(receiver.port());
        expect(setupStreams(other, {101}), "a stream set up on a second connection");
    }
    expect(eventually([&] { return receiver.openStreamCount() == 0; }),
           "a control connection closing ends the session's streams");
    receiver.stop();
}

// Where the phone's buffered stream is playing, as /feedback reports it.
void testFeedbackSampleTime()
{
    using airplay::feedbackSampleTime;
    constexpr int64_t kSecond = 1000000000;
    expect(feedbackSampleTime(1000, 0, kSecond / 2, 44100, 1000) == 1000,
           "inside the playout latency, the first sample is still the one playing");
    expect(feedbackSampleTime(1000, 0, kSecond * 3 / 2, 44100, 1000) == 1000 + 22050,
           "half a second past the latency is half a second of samples on");
    expect(feedbackSampleTime(0xFFFFFFF0u, 0, kSecond, 1000, 0) == 1000u - 16u,
           "the position wraps as RTP timestamps do");
    const int64_t three_weeks = 21LL * 24 * 3600 * kSecond;
    expect(feedbackSampleTime(0, 0, three_weeks, 48000, 0) ==
               static_cast<uint32_t>(21ULL * 24 * 3600 * 48000),
           "and stays exact on a stream that has run for weeks");
}

// The buffered stream (102) is clock-driven: /feedback carries where it is
// playing. The low-latency streams are answered with type and rate only.
void testFeedbackAnchor(const std::filesystem::path& state_dir)
{
    airplay::Receiver receiver(makeConfig(state_dir));
    expect(receiver.start(), "the feedback receiver starts");
    Client phone(receiver.port());
    plist::Value session = plist::Value::dict();
    session.set("name", plist::Value::string("Test Phone"));
    phone.request("SETUP", "rtsp://[::1]/1234", encode(session), kPlist);

    plist::Value buffered = plist::Value::dict();
    buffered.set("type", plist::Value::integer(102));
    buffered.set("audioFormat", plist::Value::integer(0x400000));
    buffered.set("audioType", plist::Value::string("media"));
    buffered.set("streamConnectionID", plist::Value::integer(9));
    buffered.set("audioLatencyMs", plist::Value::integer(1000));
    plist::Value prompt = plist::Value::dict();
    prompt.set("type", plist::Value::integer(101));
    prompt.set("audioFormat", plist::Value::integer(0x800));
    prompt.set("audioType", plist::Value::string("default"));
    prompt.set("streamConnectionID", plist::Value::integer(10));
    plist::Value body = plist::Value::dict();
    body.set("streams", plist::Value::array({buffered, prompt}));
    const auto setup = phone.request("SETUP", "rtsp://[::1]/1234", encode(body), kPlist);
    expect(setup && setup->status == 200, "the buffered stream is set up");
    if (!setup)
    {
        return;
    }
    const auto reply = plist::decodeBinary(airplay::Bytes(setup->body.begin(), setup->body.end()));
    int64_t data_port = 0;
    int64_t prompt_port = 0;
    if (reply && reply->find("streams") != nullptr)
    {
        const plist::Value& streams = *reply->find("streams");
        for (size_t i = 0; i < streams.size(); ++i)
        {
            const int64_t type = streams.at(i).find("type")->asInteger();
            (type == 102 ? data_port : prompt_port) = streams.at(i).find("dataPort")->asInteger();
        }
    }
    expect(data_port > 0 && prompt_port > 0, "with data ports");

    // A packet as the phone seals it: RTP timestamp 5000, the stream's own key
    // (no pair-verify here, so the empty secret), AAD the timestamp+SSRC.
    const auto seal = [](const std::string& connection_id) {
        const airplay::Bytes key = airplay::crypto::hkdfSha512(
            {}, "DataStream-Salt" + connection_id, "DataStream-Output-Encryption-Key", 32);
        airplay::Bytes packet = {0x80, 0x60, 0x00, 0x01, 0x00, 0x00, 0x13, 0x88, 0x11, 0x22, 0x33, 0x44};
        const airplay::Bytes nonce = airplay::crypto::nonce64(1);
        const airplay::Bytes aad(packet.begin() + 4, packet.begin() + 12);
        const airplay::Bytes sealed = airplay::crypto::chachaSeal(key, nonce, airplay::Bytes(32, 0), aad);
        packet.insert(packet.end(), sealed.begin(), sealed.end());
        packet.insert(packet.end(), nonce.begin() + 4, nonce.end());
        return packet;
    };
    const airplay::Bytes packet = seal("9");
    const airplay::Bytes prompt_packet = seal("10");
    const int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    sockaddr_in6 to{};
    to.sin6_family = AF_INET6;
    to.sin6_port = htons(static_cast<uint16_t>(data_port));
    to.sin6_addr = in6addr_loopback;
    sockaddr_in6 to_prompt = to;
    to_prompt.sin6_port = htons(static_cast<uint16_t>(prompt_port));

    std::optional<int64_t> sample_time;
    bool prompt_plain = false;
    int64_t echoed_id = 0;
    const bool anchored = eventually([&] {
        ::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        ::sendto(fd, prompt_packet.data(), prompt_packet.size(), 0,
                 reinterpret_cast<sockaddr*>(&to_prompt), sizeof(to_prompt));
        const auto feedback = phone.request("POST", "/feedback");
        if (!feedback || feedback->body.empty())
        {
            return false;
        }
        const auto parsed =
            plist::decodeBinary(airplay::Bytes(feedback->body.begin(), feedback->body.end()));
        const plist::Value* streams = parsed ? parsed->find("streams") : nullptr;
        for (size_t i = 0; streams != nullptr && i < streams->size(); ++i)
        {
            const plist::Value& entry = streams->at(i);
            if (entry.find("type")->asInteger() == 102 && entry.find("sampleTime") != nullptr)
            {
                sample_time = entry.find("sampleTime")->asInteger();
                echoed_id = entry.find("streamConnectionID")->asInteger();
            }
            if (entry.find("type")->asInteger() == 101)
            {
                prompt_plain = entry.find("sampleTime") == nullptr && entry.find("timestamp") == nullptr;
            }
        }
        return sample_time.has_value();
    });
    ::close(fd);
    expect(anchored, "once the buffered stream has a packet, /feedback anchors it");
    expect(sample_time && *sample_time == 5000,
           "at its first sample while still inside the negotiated latency");
    expect(echoed_id == 9, "naming the stream it describes");
    expect(prompt_plain, "and a low-latency stream is answered with its type and rate only");
    receiver.stop();
}

}  // namespace

int main()
{
    const std::filesystem::path state_dir =
        std::filesystem::temp_directory_path() /
        ("airplay_test_receiver_" + std::to_string(::getpid()));
    std::filesystem::create_directories(state_dir);

    {
        airplay::Receiver receiver(makeConfig(state_dir));
        if (!receiver.start())
        {
            SPDLOG_ERROR("FAIL: the receiver did not start on [::1]");
            return 1;
        }
        expect(receiver.port() != 0, "port 0 binds an ephemeral port and reports it");

        testRouting(receiver.port());

        receiver.stop();
    }
    testSessionLost(state_dir);
    testStreamLifetime(state_dir);
    testFeedbackSampleTime();
    testFeedbackAnchor(state_dir);

    std::filesystem::remove_all(state_dir);
    if (failures == 0)
    {
        SPDLOG_INFO("all receiver checks passed");
    }
    return failures == 0 ? 0 : 1;
}
