// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/receiver.h"

#include "airplay/channel_crypto.h"
#include "airplay/info_plist.h"
#include "airplay/event_channel.h"
#include "airplay/media_stream.h"
#include "airplay/net.h"
#include "airplay/pairing_session.h"

#include "airplay/aac_decoder.h"
#include "airplay/event_queue.h"
#include "plist/binary.h"
#include "airplay/nalu.h"
#include "airplay/srp.h"
#include "airplay/tlv8.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace airplay
{
namespace
{

// TLV8 types used by pair-setup / pair-verify (HAP numbering).
constexpr uint8_t kTlvMethod = 0x00;
constexpr uint8_t kTlvIdentifier = 0x01;
constexpr uint8_t kTlvSalt = 0x02;
constexpr uint8_t kTlvPublicKey = 0x03;
constexpr uint8_t kTlvProof = 0x04;
constexpr uint8_t kTlvEncryptedData = 0x05;
constexpr uint8_t kTlvState = 0x06;
constexpr uint8_t kTlvError = 0x07;
constexpr uint8_t kTlvSignature = 0x0A;


std::string hexPreview(const Bytes& data, size_t limit = 64)
{
    std::string out;
    const size_t n = std::min(limit, data.size());
    out.reserve(n * 3);
    for (size_t i = 0; i < n; ++i)
    {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x ", data[i]);
        out += buf;
    }
    if (data.size() > n)
    {
        out += "...";
    }
    return out;
}

void logTlv(const char* direction, const Bytes& body)
{
    const auto items = tlv8::decode(body);
    if (items.empty())
    {
        return;
    }
    for (const auto& [type, value] : items)
    {
        const char* name = "?";
        switch (type)
        {
            case kTlvMethod: name = "Method"; break;
            case kTlvIdentifier: name = "Identifier"; break;
            case kTlvSalt: name = "Salt"; break;
            case kTlvPublicKey: name = "PublicKey"; break;
            case kTlvProof: name = "Proof"; break;
            case kTlvEncryptedData: name = "EncryptedData"; break;
            case kTlvState: name = "State"; break;
            case kTlvError: name = "Error"; break;
            case kTlvSignature: name = "Signature"; break;
            default: break;
        }
        SPDLOG_DEBUG("[airplay]   {} TLV {:#04x} {:<14} {} bytes: {}", direction, type, name,
                     value.size(), hexPreview(value, 16));
    }
}

// A CarPlay audioFormat is a single set bit selecting a PCM rate/channel combo.
// Returns false when the chosen format is not one of the LPCM options (i.e. the
// phone picked AAC-LC or OPUS, which we do not decode yet).
struct PcmFormat
{
    uint32_t sample_rate;
    uint8_t channels;
};

bool decodePcmFormat(int64_t format, PcmFormat& out)
{
    switch (format)
    {
        case 0x4: out = {8000, 1}; return true;
        case 0x8: out = {8000, 2}; return true;
        case 0x10: out = {16000, 1}; return true;
        case 0x20: out = {16000, 2}; return true;
        case 0x40: out = {24000, 1}; return true;
        case 0x80: out = {24000, 2}; return true;
        case 0x100: out = {32000, 1}; return true;
        case 0x200: out = {32000, 2}; return true;
        case 0x400: out = {44100, 1}; return true;
        case 0x800: out = {44100, 2}; return true;
        case 0x4000: out = {48000, 1}; return true;
        case 0x8000: out = {48000, 2}; return true;
        default: return false;
    }
}

// AAC-LC formats: 0x400000 is 44.1 kHz stereo, 0x800000 is 48 kHz stereo -- the
// entertainment (type 102) stream. Detected as bit tests because the phone may
// OR the format with other capability bits.
constexpr int64_t kAacLc44kStereo = 0x400000;
constexpr int64_t kAacLc48kStereo = 0x800000;

bool decodeAacFormat(int64_t format, PcmFormat& out)
{
    if ((format & kAacLc48kStereo) != 0)
    {
        out = {48000, 2};
        return true;
    }
    if ((format & kAacLc44kStereo) != 0)
    {
        out = {44100, 2};
        return true;
    }
    return false;
}

// Compact recursive dump of a binary plist, for bring-up logging.
void describePlist(const plist::Value& value, const std::string& indent, const std::string& key)
{
    const std::string prefix = key.empty() ? indent : indent + key + " = ";
    switch (value.type())
    {
        case plist::Value::Type::Dict:
            SPDLOG_DEBUG("[airplay] {}{{", prefix);
            for (size_t i = 0; i < value.keys().size(); ++i)
            {
                describePlist(value.valueAt(i), indent + "  ", value.keys()[i]);
            }
            SPDLOG_DEBUG("[airplay] {}}}", indent);
            break;
        case plist::Value::Type::Array:
            SPDLOG_DEBUG("[airplay] {}[", prefix);
            for (size_t i = 0; i < value.size(); ++i)
            {
                describePlist(value.valueAt(i), indent + "  ", {});
            }
            SPDLOG_DEBUG("[airplay] {}]", indent);
            break;
        case plist::Value::Type::Data:
            SPDLOG_DEBUG("[airplay] {}<{} bytes>", prefix, value.asData().size());
            break;
        case plist::Value::Type::String:
            SPDLOG_DEBUG("[airplay] {}\"{}\"", prefix, value.asString());
            break;
        case plist::Value::Type::Integer:
            SPDLOG_DEBUG("[airplay] {}{}", prefix, value.asInteger());
            break;
        case plist::Value::Type::Bool:
            SPDLOG_DEBUG("[airplay] {}{}", prefix, value.asBool());
            break;
        case plist::Value::Type::Real:
            SPDLOG_DEBUG("[airplay] {}{}", prefix, value.asReal());
            break;
        case plist::Value::Type::Date:
            SPDLOG_DEBUG("[airplay] {}<date {}>", prefix, value.asDate());
            break;
        case plist::Value::Type::Null:
            SPDLOG_DEBUG("[airplay] {}<null>", prefix);
            break;
    }
}

}  // namespace

// The receiver's session state. Held behind a pointer so the header stays free
// of the socket and crypto types.
struct Receiver::State
{
    // The three handshakes, and the accessory identity they establish.
    PairingSession pairing;

    State(PairingSession::Config pairing_config, EventChannel::Config event_config) :
        pairing(std::move(pairing_config)), events(std::move(event_config))
    {
    }

    // Clock sync against the phone. Mandatory: without it the phone tears the
    // session down a few seconds after RECORD.
    TimingSync timing;

    // Peer of the control connection, needed to aim the timing sync. The
    // address is link-local, so the scope id matters.
    std::string peer_address;
    uint32_t peer_scope = 0;

    // The event channel: input out to the phone, its own commands back.
    EventChannel events;
    uint16_t event_port = 0;

    // Per-stream data listeners.
    std::vector<int> stream_fds;

    // Where a low-power phone sends its keepalive datagrams. Never read: the
    // socket exists so the datagrams have somewhere to land.
    int keep_alive_fd = -1;
    uint16_t keep_alive_port = 0;

    // The audio streams the phone currently has open, which is what POST
    // /feedback has to report back. Touched by the RTSP session thread only,
    // but /feedback and SETUP may arrive on different connections.
    struct AudioStreamInfo
    {
        int64_t type = 0;
        uint32_t sample_rate = 0;
    };
    std::mutex audio_mutex;
    std::vector<AudioStreamInfo> audio_streams;

    // Microphone uplink (us -> phone), set up when the phone's main-audio SETUP
    // carries a dataPort of its own. Guarded because feedMic() runs on the zenoh
    // subscriber thread while setup/teardown run on the RTSP session thread.
    std::mutex mic_mutex;
    bool mic_active = false;
    int mic_fd = -1;
    sockaddr_in6 mic_dest{};
    Bytes mic_key;
    uint32_t mic_sample_rate = 0;
    uint8_t mic_channels = 0;
    int mic_payload_type = 100;
    size_t mic_samples_per_frame = 0;  // PCM framing granularity
    Bytes mic_accum;                   // leftover PCM between feed() calls
    uint16_t mic_seq = 0;
    uint32_t mic_ts = 0;
    uint64_t mic_nonce = 0;
};

Receiver::Receiver(ReceiverConfig config) : config_(std::move(config))
{
    PairingSession::Config pairing;
    // Both handshakes sign over this, and it must be the same string GET /info
    // advertises as `name` -- the phone checks them against each other.
    pairing.identifier = config_.name;
    pairing.mfi_certificate = config_.mfi_certificate;
    pairing.mfi_sign = config_.mfi_sign;
    pairing.mfi_protocol_major = config_.mfi_protocol_major;
    EventChannel::Config events;
    events.width = config_.width;
    events.height = config_.height;
    events.display_uuid = kMainDisplayUuid;
    state_ = std::make_unique<State>(std::move(pairing), std::move(events));

    // Inbound commands are routed by the receiver: what a requestUI means is
    // session policy, not transport.
    state_->events.setCommandHandler(
        [this](const rtsp::Message& request) { return handleEventCommand(request); });
}

Receiver::~Receiver()
{
    stop();
}

void Receiver::setVideoHandler(VideoHandler handler)
{
    video_handler_ = std::move(handler);
}

void Receiver::setAudioHandler(AudioHandler handler)
{
    audio_handler_ = std::move(handler);
}

void Receiver::setStatusHandler(StatusHandler handler)
{
    status_handler_ = std::move(handler);
}

void Receiver::setMicStatusHandler(MicStatusHandler handler)
{
    mic_status_handler_ = std::move(handler);
}

void Receiver::setOemButtonHandler(OemButtonHandler handler)
{
    oem_button_handler_ = std::move(handler);
}

bool Receiver::start()
{
    if (run_.load())
    {
        return true;
    }

    server_fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd_ < 0)
    {
        SPDLOG_ERROR("[airplay] socket() failed: {}", std::strerror(errno));
        return false;
    }

    int on = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    // Accept IPv4-mapped too; harmless and avoids surprises on other links.
    int off = 0;
    ::setsockopt(server_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(config_.port);
    addr.sin6_addr = in6addr_any;

    // Binding one address rather than the wildcard, when asked to.
    //
    // This is not fussiness. On macOS the system AirPlay Receiver (inside
    // ControlCenter) already holds *:7000, so the wildcard bind fails outright
    // with EADDRINUSE -- but a bind to one specific address on the same port
    // succeeds alongside it, given SO_REUSEADDR above. Since the phone only
    // ever dials the link-local we advertised, binding exactly that is both the
    // narrower thing to do and the only thing that works there.
    //
    // Resolved with getaddrinfo rather than inet_pton because a link-local is
    // meaningless without its scope, and only getaddrinfo understands the
    // "fe80::1%en9" form that carries it.
    if (!config_.bind_address.empty())
    {
        addrinfo hints{};
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST;
        addrinfo* resolved = nullptr;
        const int rc = ::getaddrinfo(config_.bind_address.c_str(), nullptr, &hints, &resolved);
        if (rc != 0 || resolved == nullptr)
        {
            SPDLOG_ERROR("[airplay] cannot parse bind address '{}': {}", config_.bind_address,
                         ::gai_strerror(rc));
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }
        const auto* wanted = reinterpret_cast<const sockaddr_in6*>(resolved->ai_addr);
        addr.sin6_addr = wanted->sin6_addr;
        addr.sin6_scope_id = wanted->sin6_scope_id;
        ::freeaddrinfo(resolved);
    }

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        SPDLOG_ERROR("[airplay] bind({}:{}) failed: {}",
                     config_.bind_address.empty() ? "[::]" : config_.bind_address.c_str(),
                     config_.port, std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    if (::listen(server_fd_, 8) < 0)
    {
        SPDLOG_ERROR("[airplay] listen() failed: {}", std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    run_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });

    // Sole writer of the event channel, so that neither the touch path nor the
    // keyframe path ever blocks on a socket write or on the other.

    // Periodically ask the phone for a fresh keyframe. Without this, a static
    // CarPlay screen produces exactly one keyframe (at session start) and then
    // only P-frames, so a renderer that subscribes to the zenoh video topic
    // late -- which the dashboard always does -- never gets a sync point.
    keyframe_thread_ = std::thread([this] {
        // Only nudge the phone when no keyframe/config has arrived recently. A
        // static screen (one keyframe, then only P-frames) needs the nudge for a
        // late-joining renderer to sync; an animated screen emits its own
        // keyframes and must not be asked for redundant ones (which doubles the
        // keyframe bandwidth for nothing).
        constexpr auto kStaleAfter = std::chrono::milliseconds(1500);
        while (run_.load())
        {
            for (int i = 0; i < 5 && run_.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const auto last = std::chrono::steady_clock::time_point(
                std::chrono::steady_clock::duration(last_keyframe_ns_.load()));
            if (std::chrono::steady_clock::now() - last >= kStaleAfter)
            {
                state_->events.requestKeyframe();
            }
        }
    });
    SPDLOG_INFO("[airplay] RTSP receiver listening on {}:{}",
                config_.bind_address.empty() ? "[::]" : config_.bind_address.c_str(),
                config_.port);
    return true;
}

void Receiver::stop()
{
    if (!run_.exchange(false))
    {
        return;
    }
    if (server_fd_ >= 0)
    {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }
    if (accept_thread_.joinable())
    {
        accept_thread_.join();
    }
    if (keyframe_thread_.joinable())
    {
        keyframe_thread_.join();
    }
    state_->events.stop();
    for (auto& thread : session_threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    session_threads_.clear();

    state_->event_port = 0;
    // The keepalive listener handleSetup opened. Closed only here, after the
    // threads are down -- and reset, so a restarted receiver opens a fresh one
    // rather than advertising a port nothing is listening on.
    if (state_->keep_alive_fd >= 0)
    {
        ::close(state_->keep_alive_fd);
        state_->keep_alive_fd = -1;
        state_->keep_alive_port = 0;
    }

    endSession("receiver stopped");
    // Unconditional, unlike endSession's: a caller that never saw RECORD still
    // wants to know the receiver is down.
    if (status_handler_)
    {
        status_handler_(false);
    }
    SPDLOG_INFO("[airplay] receiver stopped");
}

void Receiver::acceptLoop()
{
    while (run_.load())
    {
        pollfd pfd{server_fd_, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 200);
        if (ready <= 0)
        {
            continue;
        }

        sockaddr_in6 peer{};
        socklen_t peer_len = sizeof(peer);
        const int client = ::accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client < 0)
        {
            if (run_.load() && errno != EINTR && errno != EAGAIN)
            {
                SPDLOG_DEBUG("[airplay] accept() failed: {}", std::strerror(errno));
            }
            continue;
        }

        char text[INET6_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof(text));

        int nodelay = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        SPDLOG_INFO("[airplay] connection from [{}]:{}", text, ntohs(peer.sin6_port));
        state_->peer_address = text;
        state_->peer_scope = peer.sin6_scope_id;
        session_threads_.emplace_back([this, client, peer_text = std::string(text)] {
            sessionLoop(client, peer_text);
            // However sessionLoop got here -- TEARDOWN, the phone unplugged, a
            // frame that failed to authenticate -- the control connection is
            // gone, and with it the session. Reporting it from one place means
            // no exit path can forget to.
            endSession("control connection closed");
        });
    }
}

void Receiver::sessionLoop(int client_fd, std::string peer)
{
    Bytes buffer;      // raw bytes off the socket
    Bytes plaintext;   // RTSP bytes, after decryption once the channel is up
    Bytes chunk(8192);
    ChannelCrypto channel;

    while (run_.load())
    {
        pollfd pfd{client_fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 200);
        if (ready < 0)
        {
            break;
        }
        if (ready == 0)
        {
            continue;
        }

        const ssize_t n = ::recv(client_fd, chunk.data(), chunk.size(), 0);
        if (n <= 0)
        {
            break;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + n);

        if (channel.active())
        {
            if (!channel.open(buffer, plaintext))
            {
                SPDLOG_ERROR("[airplay] control channel frame failed to authenticate; "
                             "closing. Suspect the key direction or the nonce counter.");
                ::close(client_fd);
                return;
            }
        }
        else
        {
            plaintext.insert(plaintext.end(), buffer.begin(), buffer.end());
            buffer.clear();
        }

        // Drain every complete request in the buffer.
        while (true)
        {
            rtsp::Message request;
            const auto consumed = rtsp::parseRequest(plaintext, request);
            if (!consumed)
            {
                SPDLOG_WARN("[airplay] malformed request from {}, closing", peer);
                buffer.clear();
                ::close(client_fd);
                return;
            }
            if (*consumed == 0)
            {
                break;  // need more bytes
            }
            plaintext.erase(plaintext.begin(), plaintext.begin() + static_cast<long>(*consumed));

            rtsp::Message response = handle(request);

            // RTSP requires the CSeq to be echoed; the phone drops responses
            // without it and simply retries, which looks like a hang.
            if (const std::string* cseq = request.header("CSeq"); cseq != nullptr)
            {
                response.setHeader("CSeq", *cseq);
            }
            response.setHeader("Server", "AirTunes/366.0");

            Bytes wire = rtsp::serializeResponse(response);

            // pair-verify M4 is the last plaintext message; everything after it
            // on this connection is encrypted, in both directions.
            const bool activates_encryption =
                !channel.active() && request.uri == "/pair-verify" && state_->pairing.verified();

            if (channel.active())
            {
                wire = channel.seal(wire);
            }

            size_t sent = 0;
            while (sent < wire.size())
            {
                const ssize_t written =
                    ::send(client_fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
                if (written <= 0)
                {
                    SPDLOG_DEBUG("[airplay] send failed: {}", std::strerror(errno));
                    ::close(client_fd);
                    return;
                }
                sent += static_cast<size_t>(written);
            }

            if (activates_encryption)
            {
                // Naming follows HAP: the *controller* reads with
                // "Control-Read-Encryption-Key", so that is our outbound key.
                channel.activate(state_->pairing.controlWriteKey(), state_->pairing.controlReadKey());
                SPDLOG_INFO("[airplay] control channel is now encrypted");
            }
        }
    }

    SPDLOG_INFO("[airplay] connection from {} closed", peer);
    ::close(client_fd);
}

rtsp::Message Receiver::handle(const rtsp::Message& request)
{
    SPDLOG_INFO("[airplay] --> {} {} ({} byte body, {})", request.method, request.uri,
                request.body.size(),
                request.contentType().empty() ? "no content-type" : request.contentType());
    for (const auto& [key, value] : request.headers)
    {
        SPDLOG_DEBUG("[airplay]     {}: {}", key, value);
    }
    if (!request.body.empty())
    {
        // Sniff the body rather than trust Content-Type: the phone labels the
        // TLV8 pair-setup/verify bodies as "application/x-apple-binary-plist"
        // too, so a header check would send TLV8 down the plist decoder and log
        // a spurious error. Real plists begin with the "bplist00" magic.
        static constexpr std::string_view kPlistMagic = "bplist00";
        const bool looks_like_plist =
            request.body.size() >= kPlistMagic.size() &&
            std::equal(kPlistMagic.begin(), kPlistMagic.end(), request.body.begin());

        if (looks_like_plist)
        {
            if (const auto parsed = plist::decodeBinary(request.body); parsed)
            {
                describePlist(*parsed, "  ", {});
            }
            else
            {
                SPDLOG_DEBUG("[airplay]   body: {}", hexPreview(request.body));
            }
        }
        else
        {
            SPDLOG_DEBUG("[airplay]   body: {}", hexPreview(request.body));
            logTlv("<-", request.body);
        }
    }

    if (request.uri == "/pair-setup")
    {
        return state_->pairing.handlePairSetup(request);
    }
    if (request.uri == "/pair-verify")
    {
        return state_->pairing.handlePairVerify(request);
    }
    if (request.uri == "/auth-setup")
    {
        return state_->pairing.handleAuthSetup(request);
    }
    if (request.method == "GET" && request.uri == "/info")
    {
        return handleInfo(request);
    }
    if (request.method == "SETUP")
    {
        return handleSetup(request);
    }
    if (request.method == "RECORD")
    {
        return handleRecord(request);
    }
    if (request.method == "TEARDOWN")
    {
        return handleTeardown(request);
    }
    if (request.method == "OPTIONS")
    {
        rtsp::Message response = rtsp::makeResponse(200, "OK", "", {});
        response.setHeader("Public",
                           "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
                           "GET_PARAMETER, SET_PARAMETER, POST, GET");
        return response;
    }
    if (request.uri == "/feedback")
    {
        return handleFeedback(request);
    }
    if (request.uri == "/command")
    {
        // A phone-initiated command arriving on the control channel rather than
        // the event one. Answering 501 makes the phone treat the session as
        // broken, so it is routed and acknowledged exactly the same way.
        return handleEventCommand(request);
    }
    if (request.method == "GET_PARAMETER" || request.method == "SET_PARAMETER" ||
        request.method == "FLUSH")
    {
        SPDLOG_INFO("[airplay] {} acknowledged", request.method);
        return rtsp::makeResponse(200, "OK", "", {});
    }

    SPDLOG_WARN("[airplay] no handler for {} {} -- answering 501", request.method, request.uri);
    return rtsp::makeResponse(501, "Not Implemented", "", {});
}



rtsp::Message Receiver::handleSetup(const rtsp::Message& request)
{
    const auto body = plist::decodeBinary(request.body);
    if (!body || !body->isDict())
    {
        SPDLOG_ERROR("[airplay] SETUP body is not a plist dict");
        return rtsp::makeResponse(400, "Bad Request", "", {});
    }

    const plist::Value* streams = body->find("streams");

    if (streams == nullptr)
    {
        // Phase 1: the session itself. The phone tells us where its timing
        // channel is and expects our event channel port in return.
        if (const plist::Value* name = body->find("name"); name != nullptr)
        {
            SPDLOG_INFO("[airplay] SETUP session for '{}' ({})", name->asString(),
                        body->find("model") != nullptr ? body->find("model")->asString() : "?");
        }

        if (state_->event_port == 0)
        {
            // Keyed from the pair-verify secret, which by now exists: the phone
            // does not reach SETUP without completing pair-verify first.
            state_->events.useSharedSecret(state_->pairing.verifySharedSecret());
            if (!state_->events.start(state_->event_port))
            {
                return rtsp::makeResponse(500, "Internal Server Error", "", {});
            }
        }
        SPDLOG_INFO("[airplay] SETUP session: advertising eventPort {}", state_->event_port);

        // Bring up our timing port and start driving the sync against theirs.
        uint16_t timing_port = 0;
        if (state_->timing.listen(timing_port))
        {
            const plist::Value* peer_timing = body->find("timingPort");
            const int64_t peer_port = peer_timing != nullptr ? peer_timing->asInteger() : 0;
            if (peer_port > 0 && !state_->peer_address.empty())
            {
                state_->timing.start(state_->peer_address, static_cast<uint16_t>(peer_port),
                                     state_->peer_scope);
            }
            else
            {
                SPDLOG_WARN("[airplay] no peer timingPort in SETUP; clock sync not started");
            }
        }

        // We advertise keepAliveLowPower in /info, so a phone that takes us up
        // on it needs somewhere to send them. Nothing reads the datagrams --
        // their arrival is the whole message -- but without a bound port the
        // phone is keeping a session alive against a closed socket.
        uint16_t keep_alive_port = 0;
        if (const plist::Value* low_power = body->find("keepAliveLowPower");
            low_power != nullptr && low_power->asBool())
        {
            if (state_->keep_alive_fd < 0)
            {
                state_->keep_alive_fd = net::openUdpSocket(state_->keep_alive_port);
            }
            keep_alive_port = state_->keep_alive_port;
        }

        plist::Value reply = plist::Value::dict();
        reply.set("timingPort", plist::Value::integer(timing_port));
        reply.set("eventPort", plist::Value::integer(state_->event_port));
        if (keep_alive_port != 0)
        {
            SPDLOG_INFO("[airplay] SETUP session: advertising keepAlivePort {}", keep_alive_port);
            reply.set("keepAlivePort", plist::Value::integer(keep_alive_port));
        }
        // Without enabledFeatures the phone has nothing to turn on and tears
        // the session down straight after RECORD.
        reply.set("enabledFeatures",
                  plist::Value::array({plist::Value::string("iAPChannel"),
                                       plist::Value::string("viewAreas")}));
        return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist",
                                  plist::encodeBinary(reply));
    }

    // Phase 2: one entry per media stream the phone wants to open.
    constexpr int64_t kStreamMainScreen = 110;
    constexpr int64_t kStreamMainAudio = 100;
    constexpr int64_t kStreamAltAudio = 101;
    constexpr int64_t kStreamMainHighAudio = 102;

    SPDLOG_INFO("[airplay] SETUP with {} stream(s)", streams->size());
    std::vector<plist::Value> out_streams;

    for (size_t i = 0; i < streams->size(); ++i)
    {
        const plist::Value& stream = streams->valueAt(i);
        const plist::Value* type = stream.find("type");
        const int64_t stream_type = type != nullptr ? type->asInteger() : -1;
        const plist::Value* connection_id = stream.find("streamConnectionID");
        const int64_t stream_connection_id =
            connection_id != nullptr ? connection_id->asInteger() : 0;

        // The phone sends streamConnectionID as an *unsigned* 64-bit value that
        // lands in the HKDF salt as a decimal string; a signed rendering of a
        // value above INT64_MAX yields the wrong key (verified on the video
        // stream). Always format it unsigned.
        const std::string connection_id_text =
            std::to_string(static_cast<uint64_t>(stream_connection_id));
        const Bytes output_key = crypto::hkdfSha512(state_->pairing.verifySharedSecret(),
                                                    "DataStream-Salt" + connection_id_text,
                                                    "DataStream-Output-Encryption-Key", 32);

        if (stream_type == kStreamMainScreen)
        {
            uint16_t data_port = 0;
            const int fd = net::openEphemeralListener(data_port);
            if (fd < 0)
            {
                return rtsp::makeResponse(500, "Internal Server Error", "", {});
            }
            SPDLOG_INFO("[airplay] video stream -> dataPort {} (connectionID {})", data_port,
                        stream_connection_id);
            session_threads_.emplace_back([this, fd, output_key] {
                runScreenStream(fd, output_key, run_, video_handler_,
                                [this](int64_t ns) { last_keyframe_ns_.store(ns); });
            });
            state_->stream_fds.push_back(fd);

            plist::Value entry = plist::Value::dict();
            entry.set("type", plist::Value::integer(stream_type));
            entry.set("dataPort", plist::Value::integer(data_port));
            out_streams.push_back(std::move(entry));
        }
        else if (stream_type == kStreamMainAudio || stream_type == kStreamAltAudio ||
                 stream_type == kStreamMainHighAudio)
        {
            const plist::Value* format = stream.find("audioFormat");
            const plist::Value* audio_type_val = stream.find("audioType");
            const std::string audio_type =
                audio_type_val != nullptr ? audio_type_val->asString() : "";

            const int64_t format_bits = format != nullptr ? format->asInteger() : 0;
            PcmFormat pcm{};
            const bool is_pcm = format != nullptr && decodePcmFormat(format_bits, pcm);
            const bool is_aac = !is_pcm && decodeAacFormat(format_bits, pcm);

            uint16_t data_port = 0;
            uint16_t control_port = 0;
            const int data_fd = net::openUdpSocket(data_port);
            const int control_fd = net::openUdpSocket(control_port);
            if (data_fd < 0 || control_fd < 0)
            {
                if (data_fd >= 0) ::close(data_fd);
                if (control_fd >= 0) ::close(control_fd);
                return rtsp::makeResponse(500, "Internal Server Error", "", {});
            }
            state_->stream_fds.push_back(data_fd);
            state_->stream_fds.push_back(control_fd);

            if (is_pcm || is_aac)
            {
                SPDLOG_INFO("[airplay] audio stream type {} '{}' -> {} {} Hz {} ch, dataPort {} "
                            "controlPort {}",
                            stream_type, audio_type, is_aac ? "AAC-LC" : "PCM", pcm.sample_rate,
                            pcm.channels, data_port, control_port);
                session_threads_.emplace_back([this, data_fd, output_key, pcm, stream_type,
                                               audio_type, is_aac] {
                    runAudioStream(data_fd, output_key, pcm.sample_rate, pcm.channels,
                                   static_cast<int>(stream_type), audio_type, is_aac, run_,
                                   audio_handler_);
                });
            }
            else
            {
                // OPUS (or an unknown format): ports are opened and answered so
                // the phone keeps the session healthy, but the stream is not
                // decoded. OPUS only appears on the wireless path; the wired one
                // we drive picks PCM/AAC.
                SPDLOG_WARN("[airplay] audio stream type {} '{}' uses an unsupported format "
                            "(0x{:x}); not decoded",
                            stream_type, audio_type, format_bits);
            }

            // Microphone uplink: a main-audio SETUP that carries the phone's own
            // dataPort means the phone wants captured mic audio sent there
            // (Siri, a call). Set it up and signal the node to start capturing.
            const plist::Value* phone_port = stream.find("dataPort");
            if (stream_type == kStreamMainAudio && is_pcm && phone_port != nullptr &&
                phone_port->asInteger() > 0 && !state_->peer_address.empty())
            {
                startMicUplink(static_cast<uint16_t>(phone_port->asInteger()), output_key,
                               pcm.sample_rate, pcm.channels, static_cast<int>(stream_type),
                               stream);
            }

            {
                // Remembered for POST /feedback, which has to name every open
                // audio stream: a phone that asks and is told nothing treats
                // the stream as dead and tears it down.
                std::lock_guard<std::mutex> lock(state_->audio_mutex);
                std::erase_if(state_->audio_streams,
                              [&](const State::AudioStreamInfo& info) {
                                  return info.type == stream_type;
                              });
                state_->audio_streams.push_back({stream_type, pcm.sample_rate});
            }

            plist::Value entry = plist::Value::dict();
            entry.set("type", plist::Value::integer(stream_type));
            entry.set("dataPort", plist::Value::integer(data_port));
            entry.set("controlPort", plist::Value::integer(control_port));
            // Echo the connection id or the phone cannot correlate the response.
            if (connection_id != nullptr)
            {
                entry.set("streamConnectionID", plist::Value::integer(stream_connection_id));
            }
            out_streams.push_back(std::move(entry));
        }
        else
        {
            // Unknown stream (e.g. the iAP data tunnel, type 130): open a TCP
            // port and answer so the phone does not tear down, but do not
            // interpret it yet.
            uint16_t data_port = 0;
            const int fd = net::openEphemeralListener(data_port);
            if (fd < 0)
            {
                return rtsp::makeResponse(500, "Internal Server Error", "", {});
            }
            state_->stream_fds.push_back(fd);
            SPDLOG_INFO("[airplay] stream type {} -> dataPort {} (not handled yet)", stream_type,
                        data_port);

            plist::Value entry = plist::Value::dict();
            entry.set("type", plist::Value::integer(stream_type));
            entry.set("dataPort", plist::Value::integer(data_port));
            if (connection_id != nullptr)
            {
                entry.set("streamConnectionID", plist::Value::integer(stream_connection_id));
            }
            out_streams.push_back(std::move(entry));
        }
    }

    plist::Value reply = plist::Value::dict();
    reply.set("streams", plist::Value::array(std::move(out_streams)));
    return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist",
                              plist::encodeBinary(reply));
}

// The phone's screen stream: a 128-byte header followed by a body whose length
// is the header's first little-endian uint32. Opcode 1 is the codec config
// (avcC, in the clear); opcode 0 is a frame, ChaCha20-Poly1305 sealed with the
// whole header as AAD and a counter nonce that advances only on frames.



rtsp::Message Receiver::handleEventCommand(const rtsp::Message& request)
{
    // Everything the phone pushes here is POST /command with a binary plist
    // body; /feedback and the rest only ever travel on the control channel.
    const auto body = plist::decodeBinary(request.body);
    if (!body || !body->isDict())
    {
        SPDLOG_DEBUG("[airplay] event channel: {} {} with no plist body", request.method,
                     request.uri);
        return rtsp::makeResponse(200, "OK", "", {});
    }

    const plist::Value* type_value = body->find("type");
    const std::string type = (type_value != nullptr && type_value->isString())
                                 ? type_value->asString()
                                 : std::string();
    const plist::Value* params = body->find("params");

    if (type == "requestUI")
    {
        if (isOemButtonPress(*body))
        {
            SPDLOG_INFO("[airplay] manufacturer button pressed -- phone is asking for the "
                        "vehicle's own UI");
            if (oem_button_handler_)
            {
                oem_button_handler_();
            }
        }
        else
        {
            // Same command, but an app naming something specific for the head
            // unit to open. Nothing consumes these yet. Getting here is exactly
            // the case isOemButtonPress rejects, so params holds a real url.
            SPDLOG_INFO("[airplay] requestUI for '{}' (not routed anywhere yet)",
                        params->find("url")->asString());
        }
    }
    else if (type == "modesChanged")
    {
        // The phone reporting who now owns the screen, the audio, and each app
        // state. The one worth tracking is speech: appStateID 1's speechMode
        // says whether Siri is listening (1) or speaking (2).
        if (params != nullptr && params->isDict())
        {
            if (const plist::Value* states = params->find("appStates");
                states != nullptr && states->isArray())
            {
                for (size_t i = 0; i < states->size(); ++i)
                {
                    const plist::Value& app_state = states->valueAt(i);
                    const plist::Value* id = app_state.find("appStateID");
                    const plist::Value* speech = app_state.find("speechMode");
                    if (id == nullptr || id->asInteger() != 1 || speech == nullptr)
                    {
                        continue;
                    }
                    const int64_t mode = speech->asInteger();
                    const bool active = (mode == 1 || mode == 2);
                    if (active != speech_active_)
                    {
                        speech_active_ = active;
                        SPDLOG_INFO("[airplay] Siri speech {}", active ? "active" : "done");
                    }
                }
            }
        }
        SPDLOG_DEBUG("[airplay] modesChanged:");
        describePlist(*body, "  ", {});
    }
    else if (type == "duckAudio" || type == "unduckAudio")
    {
        // The phone asking the head unit to attenuate *its own* audio sources
        // under a navigation prompt or a call. Logged rather than acted on:
        // this head unit has no source of its own to duck yet -- the phone
        // mixes its music and prompts together before sending them to us.
        double volume_db = 0.0;
        int64_t duration_ms = 0;
        if (params != nullptr && params->isDict())
        {
            if (const plist::Value* value = params->find("volume"); value != nullptr)
            {
                volume_db = value->asReal();
            }
            if (const plist::Value* value = params->find("durationMs"); value != nullptr)
            {
                duration_ms = value->asInteger();
            }
        }
        const double level = (type == "duckAudio") ? std::pow(10.0, volume_db / 20.0) : 1.0;
        SPDLOG_INFO("[airplay] {} to level {:.3f} over {} ms (no head-unit audio to duck)",
                    type, level, duration_ms);
    }
    else if (type == "suggestUI")
    {
        // URLs the phone offers the head unit's own UI. Nothing here shows
        // them; the dashboard decides what it displays.
        size_t urls = 0;
        if (params != nullptr && params->isDict())
        {
            if (const plist::Value* list = params->find("urls");
                list != nullptr && list->isArray())
            {
                urls = list->size();
            }
        }
        SPDLOG_INFO("[airplay] suggestUI with {} url(s) (not shown)", urls);
    }
    else if (type == "disableBluetooth")
    {
        // On a wired session iAP2 already runs over USB, so there is no
        // Bluetooth link of ours for the phone to be asking us to drop.
        SPDLOG_INFO("[airplay] disableBluetooth (no Bluetooth link on the wired path)");
    }
    else if (!type.empty())
    {
        // Not silently dropped: an unrecognised command is how a protocol
        // feature announces itself, and the body is what identifies it.
        SPDLOG_INFO("[airplay] event command '{}' has no handler (acknowledged)", type);
        describePlist(*body, "  ", {});
    }

    return rtsp::makeResponse(200, "OK", "", {});
}














void Receiver::startMicUplink(uint16_t phone_port, const Bytes& /*shared_key*/,
                              uint32_t sample_rate, uint8_t channels, int stream_type,
                              const plist::Value& stream)
{
    std::lock_guard<std::mutex> lock(state_->mic_mutex);
    if (state_->mic_fd >= 0)
    {
        return;  // already up
    }

    state_->mic_fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (state_->mic_fd < 0)
    {
        SPDLOG_ERROR("[audio] mic uplink socket() failed: {}", std::strerror(errno));
        return;
    }

    state_->mic_dest = {};
    state_->mic_dest.sin6_family = AF_INET6;
    state_->mic_dest.sin6_port = htons(phone_port);
    state_->mic_dest.sin6_scope_id = state_->peer_scope;
    if (::inet_pton(AF_INET6, state_->peer_address.c_str(), &state_->mic_dest.sin6_addr) != 1)
    {
        SPDLOG_ERROR("[audio] mic uplink: cannot parse peer '{}'", state_->peer_address);
        ::close(state_->mic_fd);
        state_->mic_fd = -1;
        return;
    }

    // The input key mirrors the downlink (output) key: same DataStream salt
    // (this stream's connection id), the Input rather than Output label.
    const plist::Value* cid = stream.find("streamConnectionID");
    const std::string cid_text =
        std::to_string(static_cast<uint64_t>(cid != nullptr ? cid->asInteger() : 0));
    state_->mic_key = crypto::hkdfSha512(state_->pairing.verifySharedSecret(), "DataStream-Salt" + cid_text,
                                         "DataStream-Input-Encryption-Key", 32);

    // Frame granularity: the phone's framesPerPacket if given, else 20 ms.
    const plist::Value* fpp = stream.find("framesPerPacket");
    const int64_t frames = fpp != nullptr ? fpp->asInteger() : 0;
    state_->mic_samples_per_frame =
        frames > 0 ? static_cast<size_t>(frames) : (sample_rate * 20 / 1000);

    state_->mic_sample_rate = sample_rate;
    state_->mic_channels = channels;
    state_->mic_payload_type = stream_type;
    state_->mic_accum.clear();
    state_->mic_seq = 0;
    state_->mic_ts = 0;
    state_->mic_nonce = 0;
    state_->mic_active = true;

    SPDLOG_INFO("[audio] mic uplink up: [{}]:{} {} Hz {} ch, {} samples/frame",
                state_->peer_address, phone_port, sample_rate, channels,
                state_->mic_samples_per_frame);

    if (mic_status_handler_)
    {
        mic_status_handler_(true, sample_rate, channels);
    }
}

void Receiver::stopMicUplink()
{
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(state_->mic_mutex);
        if (state_->mic_fd >= 0)
        {
            ::close(state_->mic_fd);
            state_->mic_fd = -1;
        }
        was_active = state_->mic_active;
        state_->mic_active = false;
        state_->mic_accum.clear();
    }
    if (was_active && mic_status_handler_)
    {
        mic_status_handler_(false, 0, 0);
    }
}

void Receiver::sendTouch(float x, float y, TouchPhase phase)
{
    state_->events.sendTouch(x, y, static_cast<EventChannel::TouchPhase>(phase));
}

void Receiver::sendKnob(const hid::KnobState& state, bool momentary)
{
    state_->events.sendKnob(state, momentary);
}

void Receiver::sendMediaKey(hid::MediaKey key)
{
    state_->events.sendMediaKey(key);
}

void Receiver::sendTelephonyKey(hid::TelephonyKey key)
{
    state_->events.sendTelephonyKey(key);
}

void Receiver::requestSiri()
{
    state_->events.requestSiri();
}

void Receiver::requestKeyframe()
{
    state_->events.requestKeyframe();
}

void Receiver::setNightMode(bool night)
{
    night_mode_.store(night);
    state_->events.setNightMode(night);
    if (!session_live_.load())
    {
        // Pushed at RECORD instead, which is the earliest the phone accepts an
        // event command; sending one before that stalls older iOS for seconds.
        SPDLOG_DEBUG("[airplay] night mode {} (no session yet; will be sent at RECORD)",
                     night ? "on" : "off");
        return;
    }
    state_->events.pushNightMode();
}

void Receiver::feedMic(const Bytes& pcm)
{
    std::lock_guard<std::mutex> lock(state_->mic_mutex);
    if (!state_->mic_active || state_->mic_fd < 0 || state_->mic_samples_per_frame == 0)
    {
        return;
    }

    state_->mic_accum.insert(state_->mic_accum.end(), pcm.begin(), pcm.end());
    const size_t frame_bytes = state_->mic_samples_per_frame * state_->mic_channels * 2;
    if (frame_bytes == 0)
    {
        return;
    }

    while (state_->mic_accum.size() >= frame_bytes)
    {
        // PCM travels big-endian on the wire; swap each sample from S16LE.
        Bytes body(state_->mic_accum.begin(), state_->mic_accum.begin() + frame_bytes);
        for (size_t k = 0; k + 1 < body.size(); k += 2)
        {
            std::swap(body[k], body[k + 1]);
        }
        state_->mic_accum.erase(state_->mic_accum.begin(),
                                state_->mic_accum.begin() + static_cast<long>(frame_bytes));

        // RTP header, mirror of the downlink layout.
        uint8_t header[12] = {};
        header[0] = 0x80;
        header[1] = static_cast<uint8_t>(state_->mic_payload_type & 0x7F);
        header[2] = static_cast<uint8_t>((state_->mic_seq >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>(state_->mic_seq & 0xFF);
        header[4] = static_cast<uint8_t>((state_->mic_ts >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>((state_->mic_ts >> 16) & 0xFF);
        header[6] = static_cast<uint8_t>((state_->mic_ts >> 8) & 0xFF);
        header[7] = static_cast<uint8_t>(state_->mic_ts & 0xFF);
        // SSRC is zero for CarPlay input streams.

        const Bytes aad(header + 4, header + 12);
        const Bytes nonce = crypto::nonce64(state_->mic_nonce);
        const Bytes sealed = crypto::chachaSeal(state_->mic_key, nonce, body, aad);

        // Wire layout: header, ciphertext+tag, then the 8-byte LE nonce.
        Bytes packet(header, header + 12);
        packet.insert(packet.end(), sealed.begin(), sealed.end());
        const Bytes nonce8(nonce.end() - 8, nonce.end());
        packet.insert(packet.end(), nonce8.begin(), nonce8.end());

        ::sendto(state_->mic_fd, packet.data(), packet.size(), 0,
                 reinterpret_cast<sockaddr*>(&state_->mic_dest), sizeof(state_->mic_dest));

        state_->mic_seq = static_cast<uint16_t>(state_->mic_seq + 1);
        state_->mic_ts += static_cast<uint32_t>(state_->mic_samples_per_frame);
        ++state_->mic_nonce;
    }
}




rtsp::Message Receiver::handleRecord(const rtsp::Message& request)
{
    (void)request;
    SPDLOG_INFO("[airplay] RECORD -- session is live");
    session_live_.store(true);
    // The phone only accepts event commands once the session has started, so
    // this is the first chance to tell it which theme to draw. Sent every
    // session, including when nothing has changed: the phone does not remember
    // ours across sessions, and day is what it assumes.
    SPDLOG_INFO("[airplay] pushing night mode {}", night_mode_.load() ? "on" : "off");
    state_->events.pushNightMode();
    if (status_handler_)
    {
        status_handler_(true);
    }
    rtsp::Message response = rtsp::makeResponse(200, "OK", "", {});
    response.setHeader("Audio-Latency", "0");
    return response;
}

void Receiver::endSession(const char* reason)
{
    // Exchange, not a plain store: several paths can reach here for the same
    // session (a TEARDOWN, then the connection closing behind it), and the
    // dashboard should be told the session ended once, not twice.
    if (!session_live_.exchange(false))
    {
        return;
    }
    SPDLOG_INFO("[airplay] session ended ({})", reason);

    stopMicUplink();
    {
        std::lock_guard<std::mutex> lock(state_->audio_mutex);
        state_->audio_streams.clear();
    }
    // Whatever is queued belongs to the session that just ended.
    state_->events.clearQueue();
    if (status_handler_)
    {
        status_handler_(false);
    }
}

rtsp::Message Receiver::handleFeedback(const rtsp::Message& request)
{
    (void)request;
    // The phone polls this as the media clock for its audio streams. An empty
    // answer reads as "that stream is gone", and the phone tears the stream
    // down and re-opens it every few seconds.
    //
    // What a full answer adds is a playback anchor -- a timestamp and the
    // sample the sink is currently playing -- which paces the phone's sending
    // to real time. We have no such anchor: the PCM is handed to the dashboard
    // over zenoh and played there, so this side does not know where playback
    // has reached. Naming the streams without inventing a position is the
    // honest half, and is what keeps them alive.
    std::vector<plist::Value> streams;
    {
        std::lock_guard<std::mutex> lock(state_->audio_mutex);
        streams.reserve(state_->audio_streams.size());
        for (const State::AudioStreamInfo& info : state_->audio_streams)
        {
            plist::Value entry = plist::Value::dict();
            entry.set("type", plist::Value::integer(info.type));
            entry.set("sampleRate", plist::Value::integer(info.sample_rate));
            streams.push_back(std::move(entry));
        }
    }

    if (streams.empty())
    {
        return rtsp::makeResponse(200, "OK", "", {});
    }

    plist::Value body = plist::Value::dict();
    body.set("streams", plist::Value::array(std::move(streams)));
    return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist",
                              plist::encodeBinary(body));
}

rtsp::Message Receiver::handleTeardown(const rtsp::Message& request)
{
    // A TEARDOWN naming streams closes just those; one with no stream list (or
    // no body at all) ends the whole session. Treating the first as the second
    // is what makes a phone that merely stops its music look like a phone that
    // went away.
    const auto body = plist::decodeBinary(request.body);
    const plist::Value* streams =
        (body && body->isDict()) ? body->find("streams") : nullptr;

    if (streams == nullptr || !streams->isArray() || streams->size() == 0)
    {
        endSession("TEARDOWN");
        return rtsp::makeResponse(200, "OK", "", {});
    }

    for (size_t i = 0; i < streams->size(); ++i)
    {
        const plist::Value* type = streams->valueAt(i).find("type");
        const int64_t stream_type = type != nullptr ? type->asInteger() : -1;
        SPDLOG_INFO("[airplay] TEARDOWN stream type {}", stream_type);

        {
            // Drop it from what /feedback reports, or we go on claiming a
            // stream the phone has just closed.
            std::lock_guard<std::mutex> lock(state_->audio_mutex);
            std::erase_if(state_->audio_streams, [&](const State::AudioStreamInfo& info) {
                return info.type == stream_type;
            });
        }

        // The stream loops themselves end when the phone closes the socket, so
        // there is nothing else to stop here -- except the mic uplink, which is
        // ours and would otherwise keep sending into a stream that is gone.
        constexpr int64_t kStreamMainAudio = 100;
        if (stream_type == kStreamMainAudio)
        {
            stopMicUplink();
            if (mic_status_handler_)
            {
                mic_status_handler_(false, 0, 0);
            }
        }
    }
    return rtsp::makeResponse(200, "OK", "", {});
}

rtsp::Message Receiver::handleInfo(const rtsp::Message& request)
{
    SPDLOG_INFO("[airplay] GET /info: advertising {}x{} @ {} fps", config_.width, config_.height,
                config_.fps);

    // The phone sends its own dictionary with the request -- what it is, what
    // it supports. Nothing reads it yet, but it is the only place the phone
    // describes itself, so it is worth having in a bring-up log.
    if (!request.body.empty())
    {
        if (const auto ask = plist::decodeBinary(request.body); ask)
        {
            SPDLOG_DEBUG("[airplay] /info request from the phone:");
            describePlist(*ask, "  ", {});
        }
    }

    return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist",
                              plist::encodeBinary(buildInfoPlist(config_)));
}

}  // namespace airplay
