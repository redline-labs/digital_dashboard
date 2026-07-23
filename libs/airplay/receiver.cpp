// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/receiver.h"

#include "airplay/plist.h"
#include "airplay/nalu.h"
#include "airplay/srp.h"
#include "airplay/tlv8.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr uint8_t kErrorAuthentication = 0x02;

using crypto::ed25519Generate;
using crypto::Ed25519Pair;

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

constexpr const char* kTlvContentType = "application/octet-stream";

// Per-direction ChaCha20-Poly1305 state for an encrypted AirPlay channel (the
// control channel after pair-verify, and the event channel). Framing is a
// 2-byte little-endian length, that many ciphertext bytes, a 16-byte tag; the
// length is the AAD and each direction counts nonces from zero.
struct ChannelCrypto
{
    bool active = false;
    Bytes inbound_key;
    Bytes outbound_key;
    uint64_t inbound_counter = 0;
    uint64_t outbound_counter = 0;
};

// CarPlay has no screen to show a setup code on, so pair-setup runs in the
// "transient" mode with a well-known password. 3939 is what Apple's own
// transient AirPlay pairing uses; overridable while that is being confirmed
// against hardware.
std::string setupPassword()
{
    if (const char* override_value = std::getenv("AIRPLAY_SETUP_PASSWORD");
        override_value != nullptr)
    {
        return override_value;
    }
    return "3939";
}

// A two-contact multitouch HID report descriptor, following LIVI's hid.ts.
// The phone will not treat a display as touch-capable unless a HID device
// backs the primaryInputDevice it advertises.
Bytes multitouchDescriptor(uint32_t x_max, uint32_t y_max)
{
    constexpr int kContacts = 2;
    Bytes out{0x05, 0x0D,   // Usage Page (Digitizers)
              0x09, 0x04,   // Usage (Touch Screen)
              0xA1, 0x01};  // Collection (Application)

    for (int i = 0; i < kContacts; ++i)
    {
        const Bytes finger{
            0x05, 0x0D,                                            // Usage Page (Digitizers)
            0x09, 0x22,                                            // Usage (Finger)
            0xA1, 0x02,                                            // Collection (Logical)
            0x09, 0x38,                                            // Usage (Transducer Index)
            0x75, 0x08,                                            // Report Size (8)
            0x95, 0x01,                                            // Report Count (1)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0x15, 0x00,                                            // Logical Minimum (0)
            0x25, 0x01,                                            // Logical Maximum (1)
            0x09, 0x33,                                            // Usage (Touch)
            0x75, 0x01,                                            // Report Size (1)
            0x95, 0x01,                                            // Report Count (1)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0x95, 0x07,                                            // Report Count (7)
            0x81, 0x03,                                            // Input (Cnst,Var,Abs) padding
            0x05, 0x01,                                            // Usage Page (Generic Desktop)
            0x26, static_cast<uint8_t>(x_max & 0xFF),
            static_cast<uint8_t>((x_max >> 8) & 0xFF),             // Logical Maximum (xMax)
            0x09, 0x30,                                            // Usage (X)
            0x75, 0x10,                                            // Report Size (16)
            0x95, 0x01,                                            // Report Count (1)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0x26, static_cast<uint8_t>(y_max & 0xFF),
            static_cast<uint8_t>((y_max >> 8) & 0xFF),             // Logical Maximum (yMax)
            0x09, 0x31,                                            // Usage (Y)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0xC0};                                                 // End Collection
        out.insert(out.end(), finger.begin(), finger.end());
    }
    out.push_back(0xC0);  // End Collection
    return out;
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
        default:
            SPDLOG_DEBUG("[airplay] {}<null>", prefix);
            break;
    }
}

Bytes tlvError(uint8_t state, uint8_t error)
{
    return tlv8::encode({{kTlvState, {state}}, {kTlvError, {error}}});
}

}  // namespace

// The receiver's pairing and session state. Held behind a pointer so the header
// stays free of the crypto types.
struct Receiver::State
{
    // pair-setup (SRP). Recreated per attempt: the phone retries from M1.
    std::unique_ptr<srp::Server> srp_server;
    Bytes srp_session_key;

    // Long-term accessory identity, used to sign pair-setup M6 and pair-verify.
    Ed25519Pair identity = ed25519Generate();

    // The phone's identity, learned in M5.
    std::string device_identifier;
    Bytes device_ltpk;

    // pair-verify ephemeral exchange.
    crypto::X25519Pair verify_ephemeral;
    Bytes device_ephemeral;
    Bytes verify_shared;
    Bytes verify_session_key;

    // MFiSAP (/auth-setup).
    crypto::X25519Pair auth_ephemeral;
    Bytes auth_shared;

    // Clock sync against the phone. Mandatory: without it the phone tears the
    // session down a few seconds after RECORD.
    TimingSync timing;

    // Peer of the control connection, needed to aim the timing sync. The
    // address is link-local, so the scope id matters.
    std::string peer_address;
    uint32_t peer_scope = 0;

    // Event channel and per-stream data listeners.
    int event_fd = -1;
    uint16_t event_port = 0;
    std::vector<int> stream_fds;

    // The accepted event-channel socket and its cipher, written to from any
    // thread by sendTouch(). Guarded because the accept loop, the receive pump
    // and the input path all touch it.
    std::mutex event_mutex;
    int event_client_fd = -1;
    ChannelCrypto event_crypto;
    int event_cseq = 0;

    // Session keys for the encrypted control channel that follows pair-verify.
    Bytes control_read;
    Bytes control_write;
    bool verified = false;

    bool paired = false;
};

Receiver::Receiver(ReceiverConfig config) :
    config_(std::move(config)), state_(std::make_unique<State>())
{
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

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        SPDLOG_ERROR("[airplay] bind([::]:{}) failed: {}", config_.port, std::strerror(errno));
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

    // Periodically ask the phone for a fresh keyframe. Without this, a static
    // CarPlay screen produces exactly one keyframe (at session start) and then
    // only P-frames, so a renderer that subscribes to the zenoh video topic
    // late -- which the dashboard always does -- never gets a sync point.
    keyframe_thread_ = std::thread([this] {
        while (run_.load())
        {
            for (int i = 0; i < 10 && run_.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            requestKeyframe();
        }
    });
    SPDLOG_INFO("[airplay] RTSP receiver listening on [::]:{}", config_.port);
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
    for (auto& thread : session_threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    session_threads_.clear();
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
        });
    }
}

// Per-connection encryption state for the control channel that pair-verify
// establishes. HAP frames each message as a 2-byte little-endian length, that
// many bytes of ciphertext, then a 16-byte Poly1305 tag. The length doubles as
// the AAD, and each direction has its own counter-based nonce starting at zero.
namespace
{

// Pulls as many complete frames out of `cipher` as are available, appending the
// plaintext to `plain`. Returns false if a frame failed to authenticate.
bool decryptFrames(ChannelCrypto& channel, Bytes& cipher, Bytes& plain)
{
    while (cipher.size() >= 2)
    {
        const size_t length = static_cast<size_t>(cipher[0]) |
                              (static_cast<size_t>(cipher[1]) << 8);
        const size_t frame = 2 + length + 16;
        if (cipher.size() < frame)
        {
            break;  // more of this frame still arriving
        }

        const Bytes aad(cipher.begin(), cipher.begin() + 2);
        const Bytes sealed(cipher.begin() + 2, cipher.begin() + static_cast<long>(frame));
        const auto opened = crypto::chachaOpen(
            channel.inbound_key, crypto::nonce64(channel.inbound_counter), sealed, aad);
        if (!opened)
        {
            return false;
        }
        ++channel.inbound_counter;
        plain.insert(plain.end(), opened->begin(), opened->end());
        cipher.erase(cipher.begin(), cipher.begin() + static_cast<long>(frame));
    }
    return true;
}

Bytes encryptFrames(ChannelCrypto& channel, const Bytes& plain)
{
    // AirPlay caps a frame's plaintext at 1024 bytes.
    constexpr size_t kMaxFrame = 1024;
    Bytes out;
    size_t offset = 0;
    while (offset < plain.size())
    {
        const size_t take = std::min(kMaxFrame, plain.size() - offset);
        const Bytes aad{static_cast<uint8_t>(take & 0xFF),
                        static_cast<uint8_t>((take >> 8) & 0xFF)};
        const Bytes chunk(plain.begin() + static_cast<long>(offset),
                          plain.begin() + static_cast<long>(offset + take));
        const Bytes sealed = crypto::chachaSeal(
            channel.outbound_key, crypto::nonce64(channel.outbound_counter), chunk, aad);
        ++channel.outbound_counter;

        out.insert(out.end(), aad.begin(), aad.end());
        out.insert(out.end(), sealed.begin(), sealed.end());
        offset += take;
    }
    return out;
}

}  // namespace

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

        if (channel.active)
        {
            if (!decryptFrames(channel, buffer, plaintext))
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
                !channel.active && request.uri == "/pair-verify" && state_->verified;

            if (channel.active)
            {
                wire = encryptFrames(channel, wire);
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
                channel.outbound_key = state_->control_read;
                channel.inbound_key = state_->control_write;
                channel.active = true;
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
        if (request.contentType().find("plist") != std::string::npos)
        {
            if (const auto parsed = plist::decode(request.body); parsed)
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
        return handlePairSetup(request);
    }
    if (request.uri == "/pair-verify")
    {
        return handlePairVerify(request);
    }
    if (request.uri == "/auth-setup")
    {
        return handleAuthSetup(request);
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
    if (request.method == "OPTIONS")
    {
        rtsp::Message response = rtsp::makeResponse(200, "OK", "", {});
        response.setHeader("Public",
                           "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
                           "GET_PARAMETER, SET_PARAMETER, POST, GET");
        return response;
    }
    if (request.uri == "/feedback" || request.uri == "/command")
    {
        // Keepalive and phone-initiated UI events. Answering 501 here makes the
        // phone treat the session as broken.
        SPDLOG_DEBUG("[airplay] {} {} acknowledged", request.method, request.uri);
        return rtsp::makeResponse(200, "OK", "", {});
    }
    if (request.method == "GET_PARAMETER" || request.method == "SET_PARAMETER" ||
        request.method == "FLUSH" || request.method == "TEARDOWN")
    {
        SPDLOG_INFO("[airplay] {} acknowledged", request.method);
        return rtsp::makeResponse(200, "OK", "", {});
    }

    SPDLOG_WARN("[airplay] no handler for {} {} -- answering 501", request.method, request.uri);
    return rtsp::makeResponse(501, "Not Implemented", "", {});
}

rtsp::Message Receiver::handlePairSetup(const rtsp::Message& request)
{
    const auto items = tlv8::decode(request.body);
    const Bytes* state_tlv = tlv8::find(items, kTlvState);
    const uint8_t state = (state_tlv != nullptr && !state_tlv->empty()) ? (*state_tlv)[0] : 0;

    SPDLOG_INFO("[airplay] pair-setup M{}", state);

    switch (state)
    {
        case 1:
        {
            // M1 -> M2. Stand up a fresh SRP server; the phone always restarts
            // the exchange from M1, so any half-finished attempt is discarded.
            state_->srp_server =
                std::make_unique<srp::Server>(srp::kPairSetupUsername, setupPassword());
            if (!state_->srp_server->valid())
            {
                SPDLOG_ERROR("[airplay] could not initialise SRP");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(2, kErrorAuthentication));
            }

            const Bytes& salt = state_->srp_server->salt();
            const Bytes& public_b = state_->srp_server->publicB();
            SPDLOG_INFO("[airplay] pair-setup M2: salt {} bytes, B {} bytes", salt.size(),
                        public_b.size());

            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlv8::encode({{kTlvState, {2}},
                                                    {kTlvPublicKey, public_b},
                                                    {kTlvSalt, salt}}));
        }

        case 3:
        {
            // M3 -> M4. The phone proves it knows the password; if our password
            // guess is wrong this is exactly where it shows up.
            if (!state_->srp_server)
            {
                SPDLOG_ERROR("[airplay] pair-setup M3 with no M1 in progress");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }
            const Bytes* client_a = tlv8::find(items, kTlvPublicKey);
            const Bytes* client_m1 = tlv8::find(items, kTlvProof);
            if (client_a == nullptr || client_m1 == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-setup M3 missing PublicKey or Proof");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            const auto result = state_->srp_server->verify(*client_a, *client_m1);
            if (!result.ok)
            {
                SPDLOG_ERROR("[airplay] pair-setup M3 proof REJECTED -- the setup password is "
                             "wrong (currently '{}'). Override with AIRPLAY_SETUP_PASSWORD.",
                             setupPassword());
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            state_->srp_session_key = result.session_key;
            SPDLOG_INFO("[airplay] pair-setup M4: proof ACCEPTED, session key {} bytes",
                        result.session_key.size());
            return rtsp::makeResponse(
                200, "OK", kTlvContentType,
                tlv8::encode({{kTlvState, {4}}, {kTlvProof, result.server_proof}}));
        }

        case 5:
        {
            // M5 -> M6: both sides hand over their long-term Ed25519 identity,
            // encrypted under a key derived from the SRP session key.
            if (state_->srp_session_key.empty())
            {
                SPDLOG_ERROR("[airplay] pair-setup M5 with no session key");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(6, kErrorAuthentication));
            }
            const Bytes* encrypted = tlv8::find(items, kTlvEncryptedData);
            if (encrypted == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-setup M5 missing EncryptedData");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(6, kErrorAuthentication));
            }

            const Bytes session_key = crypto::hkdfSha512(
                state_->srp_session_key, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);

            const auto plain =
                crypto::chachaOpen(session_key, crypto::nonceLabel("PS-Msg05"), *encrypted);
            if (!plain)
            {
                SPDLOG_ERROR("[airplay] pair-setup M5 decryption failed");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(6, kErrorAuthentication));
            }

            const auto inner = tlv8::decode(*plain);
            const Bytes* device_id = tlv8::find(inner, kTlvIdentifier);
            const Bytes* device_ltpk = tlv8::find(inner, kTlvPublicKey);
            SPDLOG_INFO("[airplay] pair-setup M5: device id {} bytes, LTPK {} bytes",
                        device_id != nullptr ? device_id->size() : 0,
                        device_ltpk != nullptr ? device_ltpk->size() : 0);
            if (device_id != nullptr)
            {
                state_->device_identifier.assign(device_id->begin(), device_id->end());
            }
            if (device_ltpk != nullptr)
            {
                state_->device_ltpk = *device_ltpk;
            }

            // M6: our identifier, our long-term public key, and a signature
            // over (accessory-x | identifier | LTPK) proving we hold the key.
            const Bytes accessory_x = crypto::hkdfSha512(state_->srp_session_key,
                                                         "Pair-Setup-Accessory-Sign-Salt",
                                                         "Pair-Setup-Accessory-Sign-Info", 32);
            const Bytes identifier(config_.name.begin(), config_.name.end());

            Bytes to_sign = accessory_x;
            to_sign.insert(to_sign.end(), identifier.begin(), identifier.end());
            to_sign.insert(to_sign.end(), state_->identity.public_key.begin(),
                           state_->identity.public_key.end());
            const Bytes signature = crypto::ed25519Sign(state_->identity.private_key, to_sign);

            const Bytes sub = tlv8::encode({{kTlvIdentifier, identifier},
                                            {kTlvPublicKey, state_->identity.public_key},
                                            {kTlvSignature, signature}});
            const Bytes sealed =
                crypto::chachaSeal(session_key, crypto::nonceLabel("PS-Msg06"), sub);

            state_->paired = true;
            SPDLOG_INFO("[airplay] pair-setup M6: sending accessory identity, PAIRED");
            return rtsp::makeResponse(
                200, "OK", kTlvContentType,
                tlv8::encode({{kTlvState, {6}}, {kTlvEncryptedData, sealed}}));
        }

        default:
            SPDLOG_WARN("[airplay] unexpected pair-setup state {}", state);
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlvError(static_cast<uint8_t>(state + 1),
                                               kErrorAuthentication));
    }
}

rtsp::Message Receiver::handlePairVerify(const rtsp::Message& request)
{
    const auto items = tlv8::decode(request.body);
    const Bytes* state_tlv = tlv8::find(items, kTlvState);
    const uint8_t state = (state_tlv != nullptr && !state_tlv->empty()) ? (*state_tlv)[0] : 0;

    SPDLOG_INFO("[airplay] pair-verify M{}", state);

    switch (state)
    {
        case 1:
        {
            // M1 -> M2. Ephemeral X25519 exchange, then prove we hold the
            // long-term key the phone learned during pair-setup.
            const Bytes* device_public = tlv8::find(items, kTlvPublicKey);
            if (device_public == nullptr || device_public->size() != 32)
            {
                SPDLOG_ERROR("[airplay] pair-verify M1 missing or malformed PublicKey");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(2, kErrorAuthentication));
            }

            state_->verify_ephemeral = crypto::x25519Generate();
            state_->device_ephemeral = *device_public;
            state_->verify_shared =
                crypto::x25519Shared(state_->verify_ephemeral.private_key, *device_public);
            if (state_->verify_shared.empty())
            {
                SPDLOG_ERROR("[airplay] pair-verify X25519 exchange failed");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(2, kErrorAuthentication));
            }

            const Bytes session_key =
                crypto::hkdfSha512(state_->verify_shared, "Pair-Verify-Encrypt-Salt",
                                   "Pair-Verify-Encrypt-Info", 32);
            state_->verify_session_key = session_key;

            const Bytes identifier(config_.name.begin(), config_.name.end());

            // Signed material is our ephemeral public key, our identifier, then
            // the phone's ephemeral public key -- in that order.
            Bytes to_sign = state_->verify_ephemeral.public_key;
            to_sign.insert(to_sign.end(), identifier.begin(), identifier.end());
            to_sign.insert(to_sign.end(), device_public->begin(), device_public->end());
            const Bytes signature = crypto::ed25519Sign(state_->identity.private_key, to_sign);

            const Bytes sub =
                tlv8::encode({{kTlvIdentifier, identifier}, {kTlvSignature, signature}});
            const Bytes sealed =
                crypto::chachaSeal(session_key, crypto::nonceLabel("PV-Msg02"), sub);

            SPDLOG_INFO("[airplay] pair-verify M2: ephemeral key exchanged, signing identity");
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlv8::encode({{kTlvState, {2}},
                                                    {kTlvPublicKey,
                                                     state_->verify_ephemeral.public_key},
                                                    {kTlvEncryptedData, sealed}}));
        }

        case 3:
        {
            // M3 -> M4. The phone proves it holds the key it gave us in
            // pair-setup M5.
            if (state_->verify_session_key.empty())
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 with no M1 in progress");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }
            const Bytes* encrypted = tlv8::find(items, kTlvEncryptedData);
            if (encrypted == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 missing EncryptedData");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            const auto plain = crypto::chachaOpen(state_->verify_session_key,
                                                  crypto::nonceLabel("PV-Msg03"), *encrypted);
            if (!plain)
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 decryption failed");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            const auto inner = tlv8::decode(*plain);
            const Bytes* identifier = tlv8::find(inner, kTlvIdentifier);
            const Bytes* signature = tlv8::find(inner, kTlvSignature);
            if (identifier == nullptr || signature == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 inner TLV incomplete");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            // Mirror image of what we signed in M2.
            Bytes signed_material = state_->device_ephemeral;
            signed_material.insert(signed_material.end(), identifier->begin(), identifier->end());
            signed_material.insert(signed_material.end(),
                                   state_->verify_ephemeral.public_key.begin(),
                                   state_->verify_ephemeral.public_key.end());

            if (!state_->device_ltpk.empty() &&
                !crypto::ed25519Verify(state_->device_ltpk, signed_material, *signature))
            {
                // Logged rather than fatal for now: a persistent pair record
                // across runs is not implemented, so the LTPK we hold is only
                // the one from this session's pair-setup.
                SPDLOG_WARN("[airplay] pair-verify M3 signature did not verify against the "
                            "stored LTPK; continuing");
            }
            else
            {
                SPDLOG_INFO("[airplay] pair-verify M3 signature verified");
            }

            // Control channel keys for everything after this point.
            state_->control_read = crypto::hkdfSha512(state_->verify_shared, "Control-Salt",
                                                      "Control-Read-Encryption-Key", 32);
            state_->control_write = crypto::hkdfSha512(state_->verify_shared, "Control-Salt",
                                                       "Control-Write-Encryption-Key", 32);
            state_->verified = true;

            SPDLOG_INFO("[airplay] pair-verify M4: VERIFIED, control channel keys derived");
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlv8::encode({{kTlvState, {4}}}));
        }

        default:
            SPDLOG_WARN("[airplay] unexpected pair-verify state {}", state);
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlvError(static_cast<uint8_t>(state + 1),
                                               kErrorAuthentication));
    }
}

rtsp::Message Receiver::handleAuthSetup(const rtsp::Message& request)
{
    // MFiSAP: one byte of mode followed by the phone's Curve25519 public key.
    if (request.body.size() != 33)
    {
        SPDLOG_ERROR("[airplay] auth-setup body is {} bytes, expected 33", request.body.size());
        return rtsp::makeResponse(400, "Bad Request", "", {});
    }
    if (!config_.mfi_certificate || !config_.mfi_sign)
    {
        SPDLOG_ERROR("[airplay] auth-setup needs the MFi coprocessor and none is wired up");
        return rtsp::makeResponse(501, "Not Implemented", "", {});
    }

    const uint8_t mode = request.body[0];
    const Bytes device_public(request.body.begin() + 1, request.body.end());
    SPDLOG_INFO("[airplay] auth-setup mode {}, device key {} bytes", mode, device_public.size());

    // Our ephemeral key, and the shared secret the media streams are keyed from.
    state_->auth_ephemeral = crypto::x25519Generate();
    state_->auth_shared =
        crypto::x25519Shared(state_->auth_ephemeral.private_key, device_public);
    if (state_->auth_shared.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup X25519 exchange failed");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    const Bytes certificate = config_.mfi_certificate();
    if (certificate.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup: coprocessor returned no certificate");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    // The coprocessor signs a digest of our public key followed by theirs. The
    // digest width follows the authentication protocol major version: 2 uses
    // SHA-1, 3 uses SHA-256. Signing the wrong width fails quietly, so this is
    // driven by what the chip reports rather than assumed.
    const int major = config_.mfi_protocol_major ? config_.mfi_protocol_major() : 2;

    // The signed message is our public key followed by theirs.
    const std::vector<Bytes> operands{state_->auth_ephemeral.public_key, device_public};
    const Bytes digest = (major >= 3) ? crypto::sha256(operands) : crypto::sha1(operands);
    SPDLOG_DEBUG("[airplay] auth-setup signing a {}-byte digest (protocol major {})",
                 digest.size(), major);

    const Bytes signature = config_.mfi_sign(digest);
    if (signature.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup: coprocessor did not sign the challenge");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    // The signature travels encrypted under AES-128-CTR -- this is what
    // crypto.h's aesCtr128 exists for. Key and IV are SHA-1 (not SHA-512) of a
    // short ASCII label prepended to the shared secret, truncated to 16 bytes.
    // The certificate is public and goes in the clear.
    const auto label = [](std::string_view text) {
        return Bytes(text.begin(), text.end());
    };
    const Bytes key_material = crypto::sha1({label("AES-KEY"), state_->auth_shared});
    const Bytes iv_material = crypto::sha1({label("AES-IV"), state_->auth_shared});
    if (key_material.size() < 16 || iv_material.size() < 16)
    {
        SPDLOG_ERROR("[airplay] auth-setup: AES key derivation failed");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }
    const Bytes aes_key(key_material.begin(), key_material.begin() + 16);
    const Bytes aes_iv(iv_material.begin(), iv_material.begin() + 16);

    const Bytes sealed_signature = crypto::aesCtr128(aes_key, aes_iv, signature);
    if (sealed_signature.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup: AES-CTR of the signature failed");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    // Layout: our public key, then the certificate and the encrypted signature,
    // each preceded by a big-endian 32-bit length.
    Bytes body = state_->auth_ephemeral.public_key;
    const auto append_be32 = [&body](size_t value) {
        body.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        body.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(value & 0xFF));
    };
    append_be32(certificate.size());
    body.insert(body.end(), certificate.begin(), certificate.end());
    append_be32(sealed_signature.size());
    body.insert(body.end(), sealed_signature.begin(), sealed_signature.end());

    SPDLOG_INFO("[airplay] auth-setup: signature {} bytes, certificate {} bytes, replying {}",
                signature.size(), certificate.size(), body.size());
    return rtsp::makeResponse(200, "OK", "application/octet-stream", std::move(body));
}

int Receiver::openEphemeralListener(uint16_t& port)
{
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    int off = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = 0;  // kernel picks
    addr.sin6_addr = in6addr_any;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 4) < 0)
    {
        ::close(fd);
        return -1;
    }

    sockaddr_in6 bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0)
    {
        ::close(fd);
        return -1;
    }
    port = ntohs(bound.sin6_port);
    return fd;
}

rtsp::Message Receiver::handleSetup(const rtsp::Message& request)
{
    const auto body = plist::decode(request.body);
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

        if (state_->event_fd < 0)
        {
            state_->event_fd = openEphemeralListener(state_->event_port);
            if (state_->event_fd < 0)
            {
                SPDLOG_ERROR("[airplay] could not open the event channel listener");
                return rtsp::makeResponse(500, "Internal Server Error", "", {});
            }
            // The phone dials this as soon as it has the port. Leaving the
            // connection sitting in the backlog unaccepted looks like a dead
            // channel from its side.
            const int listen_fd = state_->event_fd;
            session_threads_.emplace_back([this, listen_fd] { eventChannelLoop(listen_fd); });
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

        plist::Value reply = plist::Value::dict();
        reply.set("timingPort", plist::Value::integer(timing_port));
        reply.set("eventPort", plist::Value::integer(state_->event_port));
        // Without enabledFeatures the phone has nothing to turn on and tears
        // the session down straight after RECORD.
        reply.set("enabledFeatures",
                  plist::Value::array({plist::Value::string("iAPChannel"),
                                       plist::Value::string("viewAreas")}));
        return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist",
                                  plist::encode(reply));
    }

    // Phase 2: one entry per media stream the phone wants to open.
    constexpr int64_t kStreamMainScreen = 110;

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

        uint16_t data_port = 0;
        const int fd = openEphemeralListener(data_port);
        if (fd < 0)
        {
            SPDLOG_ERROR("[airplay] could not open a data listener for stream type {}",
                         stream_type);
            return rtsp::makeResponse(500, "Internal Server Error", "", {});
        }

        SPDLOG_INFO("[airplay] stream type {} -> dataPort {} (connectionID {})", stream_type,
                    data_port, stream_connection_id);

        if (stream_type == kStreamMainScreen)
        {
            // Frames are sealed with a key derived from the pair-verify shared
            // secret and this stream's connection id.
            // The phone sends streamConnectionID as an *unsigned* 64-bit value
            // and it lands in the HKDF salt as a decimal string. Roughly half of
            // all sessions produce one above INT64_MAX, which our plist decoder
            // surfaces as a negative number -- rendering that signed yields a
            // different salt, a different key, and every frame failing to
            // decrypt. Verified on hardware: 4663436911794014275 worked while
            // -3498692594036096197 (really 14948051479673455419) did not.
            const auto connection_id_text =
                std::to_string(static_cast<uint64_t>(stream_connection_id));
            const Bytes key = crypto::hkdfSha512(state_->verify_shared,
                                                 "DataStream-Salt" + connection_id_text,
                                                 "DataStream-Output-Encryption-Key", 32);
            session_threads_.emplace_back(
                [this, fd, key] { screenStreamLoop(fd, key); });
        }

        plist::Value entry = plist::Value::dict();
        entry.set("type", plist::Value::integer(stream_type));
        entry.set("dataPort", plist::Value::integer(data_port));
        out_streams.push_back(std::move(entry));

        state_->stream_fds.push_back(fd);
    }

    plist::Value reply = plist::Value::dict();
    reply.set("streams", plist::Value::array(std::move(out_streams)));
    return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist",
                              plist::encode(reply));
}

// The phone's screen stream: a 128-byte header followed by a body whose length
// is the header's first little-endian uint32. Opcode 1 is the codec config
// (avcC, in the clear); opcode 0 is a frame, ChaCha20-Poly1305 sealed with the
// whole header as AAD and a counter nonce that advances only on frames.
void Receiver::screenStreamLoop(int listen_fd, Bytes key)
{
    constexpr size_t kHeaderLength = 128;
    constexpr size_t kMaxBody = 8 * 1024 * 1024;
    constexpr uint8_t kOpVideoFrame = 0;
    constexpr uint8_t kOpVideoConfig = 1;

    while (run_.load())
    {
        pollfd listen_pfd{listen_fd, POLLIN, 0};
        if (::poll(&listen_pfd, 1, 200) <= 0)
        {
            continue;
        }
        const int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0)
        {
            continue;
        }
        SPDLOG_INFO("[video] screen stream connected");

        Bytes buffer;
        Bytes chunk(65536);
        uint64_t counter = 0;
        uint64_t frames = 0;

        // Optional raw Annex-B dump for bring-up: `ffmpeg -i dump.h264 out.png`
        // turns it into a viewable image without a running dashboard.
        std::FILE* dump = nullptr;
        if (const char* path = std::getenv("AIRPLAY_DUMP_VIDEO"); path != nullptr)
        {
            dump = std::fopen(path, "wb");
            SPDLOG_INFO("[video] dumping Annex-B to {}", path);
        }

        while (run_.load())
        {
            pollfd pfd{client, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, 200);
            if (ready < 0)
            {
                break;
            }
            if (ready == 0)
            {
                continue;
            }
            const ssize_t n = ::recv(client, chunk.data(), chunk.size(), 0);
            if (n <= 0)
            {
                break;
            }
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + n);

            while (buffer.size() >= kHeaderLength)
            {
                const size_t body_size = static_cast<size_t>(buffer[0]) |
                                         (static_cast<size_t>(buffer[1]) << 8) |
                                         (static_cast<size_t>(buffer[2]) << 16) |
                                         (static_cast<size_t>(buffer[3]) << 24);
                if (body_size > kMaxBody)
                {
                    SPDLOG_ERROR("[video] implausible body size {}, dropping connection",
                                 body_size);
                    buffer.clear();
                    break;
                }
                if (buffer.size() < kHeaderLength + body_size)
                {
                    break;
                }

                const Bytes header(buffer.begin(), buffer.begin() + kHeaderLength);
                const Bytes body(buffer.begin() + kHeaderLength,
                                 buffer.begin() + kHeaderLength + body_size);
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<long>(kHeaderLength + body_size));

                const uint8_t opcode = header[4];
                if (opcode == kOpVideoConfig)
                {
                    const auto config = nalu::configToAnnexB(body);
                    if (!config)
                    {
                        SPDLOG_WARN("[video] could not parse the codec config ({} bytes)",
                                    body.size());
                        continue;
                    }
                    SPDLOG_INFO("[video] codec config: {} ({} bytes Annex-B)",
                                config->codec == nalu::Codec::H265 ? "H.265" : "H.264",
                                config->annex_b.size());
                    if (dump != nullptr)
                    {
                        std::fwrite(config->annex_b.data(), 1, config->annex_b.size(), dump);
                    }
                    if (video_handler_)
                    {
                        VideoPacket packet;
                        packet.data = config->annex_b;
                        packet.is_config = true;
                        video_handler_(packet);
                    }
                }
                else if (opcode == kOpVideoFrame)
                {
                    Bytes payload = body;
                    if (body.size() >= 16)
                    {
                        const auto opened =
                            crypto::chachaOpen(key, crypto::nonce64(counter), body, header);
                        if (!opened)
                        {
                            SPDLOG_WARN("[video] frame {} failed to decrypt", counter);
                            continue;
                        }
                        ++counter;
                        payload = *opened;
                    }

                    const Bytes annex_b = nalu::avccFrameToAnnexB(payload);
                    if (dump != nullptr)
                    {
                        std::fwrite(annex_b.data(), 1, annex_b.size(), dump);
                    }
                    if (++frames == 1)
                    {
                        SPDLOG_INFO("[video] FIRST FRAME decoded: {} bytes Annex-B",
                                    annex_b.size());
                    }
                    else if (frames % 100 == 0)
                    {
                        SPDLOG_INFO("[video] {} frames received", frames);
                    }
                    if (video_handler_)
                    {
                        VideoPacket packet;
                        packet.data = annex_b;
                        packet.keyframe = nalu::annexBContainsKeyframe(annex_b, nalu::Codec::H264);
                        video_handler_(packet);
                    }
                }
            }
        }

        if (dump != nullptr)
        {
            std::fclose(dump);
        }
        SPDLOG_INFO("[video] screen stream closed after {} frames", frames);
        ::close(client);
    }
}

void Receiver::eventChannelLoop(int listen_fd)
{
    while (run_.load())
    {
        pollfd pfd{listen_fd, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0)
        {
            continue;
        }
        const int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0)
        {
            continue;
        }
        SPDLOG_INFO("[airplay] event channel connected");

        // Events keys are derived from the pair-verify shared secret and, unlike
        // the control channel, are NOT swapped: the accessory writes with
        // Events-Write and reads with Events-Read.
        {
            std::lock_guard<std::mutex> lock(state_->event_mutex);
            state_->event_client_fd = client;
            state_->event_crypto = {};
            state_->event_crypto.outbound_key = crypto::hkdfSha512(
                state_->verify_shared, "Events-Salt", "Events-Write-Encryption-Key", 32);
            state_->event_crypto.inbound_key = crypto::hkdfSha512(
                state_->verify_shared, "Events-Salt", "Events-Read-Encryption-Key", 32);
            state_->event_crypto.active = true;
        }

        Bytes cipher;
        Bytes chunk(8192);
        while (run_.load())
        {
            pollfd cpfd{client, POLLIN, 0};
            if (::poll(&cpfd, 1, 200) <= 0)
            {
                continue;
            }
            const ssize_t n = ::recv(client, chunk.data(), chunk.size(), 0);
            if (n <= 0)
            {
                break;
            }
            cipher.insert(cipher.end(), chunk.begin(), chunk.begin() + n);

            Bytes plain;
            {
                std::lock_guard<std::mutex> lock(state_->event_mutex);
                if (!decryptFrames(state_->event_crypto, cipher, plain))
                {
                    SPDLOG_WARN("[airplay] event channel frame failed to authenticate");
                    break;
                }
            }
            // Inbound event traffic (the phone's own commands) is not acted on
            // yet; the channel exists so we can push HID reports to it.
            if (!plain.empty())
            {
                SPDLOG_DEBUG("[airplay] event channel: {} plaintext bytes", plain.size());
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_->event_mutex);
            state_->event_client_fd = -1;
            state_->event_crypto.active = false;
        }
        ::close(client);
        SPDLOG_INFO("[airplay] event channel closed");
    }
}

void Receiver::sendTouch(float x, float y, bool down)
{
    // Two-contact multitouch report: six bytes per finger --
    // [transducer index, down, x-lo, x-hi, y-lo, y-hi] -- matching the HID
    // descriptor advertised in /info. We only ever drive contact 0.
    constexpr int kContacts = 2;
    constexpr int kBytesPerFinger = 6;

    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const uint16_t px = static_cast<uint16_t>(clamp01(x) * static_cast<float>(config_.width));
    const uint16_t py = static_cast<uint16_t>(clamp01(y) * static_cast<float>(config_.height));

    Bytes report(kBytesPerFinger * kContacts, 0);
    for (int i = 0; i < kContacts; ++i)
    {
        report[i * kBytesPerFinger] = static_cast<uint8_t>(i);  // transducer index
    }
    report[1] = down ? 0x01 : 0x00;
    report[2] = static_cast<uint8_t>(px & 0xFF);
    report[3] = static_cast<uint8_t>((px >> 8) & 0xFF);
    report[4] = static_cast<uint8_t>(py & 0xFF);
    report[5] = static_cast<uint8_t>((py >> 8) & 0xFF);

    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("hidSendReport"));
    command.set("uuid", plist::Value::string("2a2a2a2a"));
    command.set("hidReport", plist::Value::data(report));
    sendEventCommand(plist::encode(command));
}

void Receiver::requestKeyframe()
{
    // forceKeyFrame names the display by the same uuid advertised in /info.
    plist::Value params = plist::Value::dict();
    params.set("uuid", plist::Value::string("b7e6c5a0-1111-4000-8000-000000000001"));
    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("forceKeyFrame"));
    command.set("params", std::move(params));
    sendEventCommand(plist::encode(command));
}

bool Receiver::sendEventCommand(const Bytes& plist_body)
{
    std::lock_guard<std::mutex> lock(state_->event_mutex);
    if (state_->event_client_fd < 0 || !state_->event_crypto.active)
    {
        return false;  // no event channel yet
    }

    const std::string head = "POST /command RTSP/1.0\r\n"
                             "Content-Type: application/x-apple-binary-plist\r\n"
                             "Content-Length: " +
                             std::to_string(plist_body.size()) +
                             "\r\nCSeq: " + std::to_string(++state_->event_cseq) + "\r\n\r\n";

    Bytes message(head.begin(), head.end());
    message.insert(message.end(), plist_body.begin(), plist_body.end());

    const Bytes wire = encryptFrames(state_->event_crypto, message);
    size_t sent = 0;
    while (sent < wire.size())
    {
        const ssize_t written = ::send(state_->event_client_fd, wire.data() + sent,
                                       wire.size() - sent, MSG_NOSIGNAL);
        if (written <= 0)
        {
            SPDLOG_DEBUG("[airplay] event channel send failed: {}", std::strerror(errno));
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

rtsp::Message Receiver::handleRecord(const rtsp::Message& request)
{
    (void)request;
    SPDLOG_INFO("[airplay] RECORD -- session is live");
    if (status_handler_)
    {
        status_handler_(true);
    }
    rtsp::Message response = rtsp::makeResponse(200, "OK", "", {});
    response.setHeader("Audio-Latency", "0");
    return response;
}

rtsp::Message Receiver::handleInfo(const rtsp::Message& request)
{
    (void)request;
    SPDLOG_INFO("[airplay] GET /info: advertising {}x{} @ {} fps", config_.width, config_.height,
                config_.fps);

    // Field names and values follow LIVI's getInfo.ts. Note the display entry
    // uses widthPixels/heightPixels and carries the *stream type* it belongs
    // to -- there is no "width"/"height"/"refreshRate" here, and getting that
    // wrong makes the phone accept the session and then tear it down without
    // ever asking for a stream.
    constexpr int64_t kStreamTypeMainScreen = 110;
    constexpr int64_t kDisplayFeatureKnobs = 0x02;
    constexpr int64_t kDisplayFeatureHighFidelityTouch = 0x08;
    constexpr int64_t kPrimaryInputTouch = 1;
    constexpr int64_t kCarplayFeatures = 0x615653aee2LL;
    constexpr int64_t kCarplayAudioFeatures = 0x10004540a00LL;

    const int64_t width_physical = 200;
    const int64_t height_physical = std::max<int64_t>(
        1, (width_physical * config_.height) / std::max<uint32_t>(1, config_.width));

    plist::Value display = plist::Value::dict();
    display.set("uuid", plist::Value::string("b7e6c5a0-1111-4000-8000-000000000001"));
    display.set("type", plist::Value::integer(kStreamTypeMainScreen));
    display.set("maxFPS", plist::Value::integer(config_.fps));
    display.set("widthPixels", plist::Value::integer(config_.width));
    display.set("heightPixels", plist::Value::integer(config_.height));
    display.set("widthPhysical", plist::Value::integer(width_physical));
    display.set("heightPhysical", plist::Value::integer(height_physical));
    display.set("features",
                plist::Value::integer(kDisplayFeatureHighFidelityTouch | kDisplayFeatureKnobs));
    display.set("primaryInputDevice", plist::Value::integer(kPrimaryInputTouch));

    // The drawable region, and inside it the region safe from occlusion. LIVI
    // always sends these, and we advertise "viewAreas" in enabledFeatures --
    // claiming the feature and then supplying no areas leaves the phone unable
    // to lay anything out. Zero insets means the whole panel is usable.
    plist::Value safe_area = plist::Value::dict();
    safe_area.set("widthPixels", plist::Value::integer(config_.width));
    safe_area.set("heightPixels", plist::Value::integer(config_.height));
    safe_area.set("originXPixels", plist::Value::integer(0));
    safe_area.set("originYPixels", plist::Value::integer(0));
    safe_area.set("drawUIOutsideSafeArea", plist::Value::boolean(true));

    plist::Value view_area = plist::Value::dict();
    view_area.set("widthPixels", plist::Value::integer(config_.width));
    view_area.set("heightPixels", plist::Value::integer(config_.height));
    view_area.set("originXPixels", plist::Value::integer(0));
    view_area.set("originYPixels", plist::Value::integer(0));
    view_area.set("safeArea", std::move(safe_area));

    display.set("viewAreas", plist::Value::array({std::move(view_area)}));
    display.set("initialViewArea", plist::Value::integer(0));

    // Resource arbitration: we are willing to hand the screen and audio over at
    // any time, at a low priority.
    const auto resource = [](int64_t id) {
        constexpr int64_t kTransferTake = 1;
        constexpr int64_t kPriorityNiceToHave = 100;
        constexpr int64_t kConstraintAnytime = 100;
        plist::Value value = plist::Value::dict();
        value.set("resourceID", plist::Value::integer(id));
        value.set("transferType", plist::Value::integer(kTransferTake));
        value.set("transferPriority", plist::Value::integer(kPriorityNiceToHave));
        value.set("takeConstraint", plist::Value::integer(kConstraintAnytime));
        value.set("borrowConstraint", plist::Value::integer(kConstraintAnytime));
        value.set("unborrowConstraint", plist::Value::integer(kConstraintAnytime));
        return value;
    };

    plist::Value app_state_speech = plist::Value::dict();
    app_state_speech.set("appStateID", plist::Value::integer(1));
    app_state_speech.set("speechMode", plist::Value::integer(-1));
    plist::Value app_state_phone = plist::Value::dict();
    app_state_phone.set("appStateID", plist::Value::integer(2));
    app_state_phone.set("state", plist::Value::boolean(false));
    plist::Value app_state_nav = plist::Value::dict();
    app_state_nav.set("appStateID", plist::Value::integer(3));
    app_state_nav.set("state", plist::Value::boolean(false));

    plist::Value modes = plist::Value::dict();
    modes.set("resources", plist::Value::array({resource(1), resource(2)}));
    modes.set("appStates", plist::Value::array({std::move(app_state_phone),
                                                std::move(app_state_speech),
                                                std::move(app_state_nav)}));

    plist::Value info = plist::Value::dict();
    // Audio capability, as LIVI advertises it. Omitting this (and masking the
    // audio feature bits) is only correct behind an explicit disable-audio
    // option; a CarPlay head unit is expected to carry audio, and a phone that
    // sees none may decline to open any stream at all.
    const auto audio_latency = [](int64_t type, const char* audio_type) {
        plist::Value value = plist::Value::dict();
        value.set("type", plist::Value::integer(type));
        value.set("inputLatencyMicros", plist::Value::integer(0));
        value.set("outputLatencyMicros", plist::Value::integer(0));
        if (audio_type != nullptr)
        {
            value.set("audioType", plist::Value::string(audio_type));
        }
        return value;
    };
    const auto audio_format = [](int64_t type, const char* audio_type, int64_t output_formats,
                                 int64_t input_formats) {
        plist::Value value = plist::Value::dict();
        value.set("type", plist::Value::integer(type));
        value.set("audioType", plist::Value::string(audio_type));
        value.set("audioOutputFormats", plist::Value::integer(output_formats));
        if (input_formats != 0)
        {
            value.set("audioInputFormats", plist::Value::integer(input_formats));
        }
        return value;
    };

    // PCM voice rates plus 44.1k media, mono and stereo (LIVI's PCM constants).
    constexpr int64_t kPcmVoice = 0x3FC;
    constexpr int64_t kPcm = kPcmVoice | 0xC00;
    constexpr int64_t kPcmMono = 0x154 | 0x400;

    info.set("audioLatencies", plist::Value::array({audio_latency(100, nullptr),
                                                    audio_latency(100, "default"),
                                                    audio_latency(100, "media"),
                                                    audio_latency(100, "telephony"),
                                                    audio_latency(100, "speechRecognition"),
                                                    audio_latency(100, "alert"),
                                                    audio_latency(101, nullptr),
                                                    audio_latency(101, "default"),
                                                    audio_latency(102, "default")}));
    info.set("audioFormats",
             plist::Value::array({audio_format(100, "compatibility", kPcm, kPcmMono),
                                  audio_format(101, "compatibility", kPcm, 0),
                                  audio_format(100, "default", kPcm, kPcmMono),
                                  audio_format(100, "alert", kPcm, 0),
                                  audio_format(100, "media", kPcm, 0),
                                  audio_format(100, "telephony", kPcmMono, kPcmMono),
                                  audio_format(100, "speechRecognition", kPcmMono, kPcmMono),
                                  audio_format(101, "default", kPcm, 0),
                                  audio_format(102, "media", kPcm, 0)}));

    info.set("sourceVersion", plist::Value::string("366.0"));
    info.set("features", plist::Value::integer(kCarplayFeatures));
    (void)kCarplayAudioFeatures;
    info.set("statusFlags", plist::Value::integer(4));
    info.set("model", plist::Value::string(config_.model));
    info.set("manufacturer", plist::Value::string("Dashboard"));
    info.set("deviceID", plist::Value::string(config_.device_id));
    info.set("bluetoothIDs", plist::Value::array({plist::Value::string(config_.device_id)}));
    info.set("name", plist::Value::string(config_.name));
    info.set("rightHandDrive", plist::Value::boolean(false));
    info.set("keepAliveLowPower", plist::Value::boolean(true));
    info.set("keepAliveSendStatsAsBody", plist::Value::boolean(false));
    info.set("modes", std::move(modes));
    info.set("extendedFeatures",
             plist::Value::array({plist::Value::string("vocoderInfo"),
                                  plist::Value::string("enhancedRequestCarUI")}));
    info.set("displays", plist::Value::array({std::move(display)}));

    plist::Value touch = plist::Value::dict();
    touch.set("hidProductID", plist::Value::integer(1));
    touch.set("hidVendorID", plist::Value::integer(2));
    touch.set("hidCountryCode", plist::Value::integer(0));
    touch.set("uuid", plist::Value::string("2a2a2a2a"));
    touch.set("name", plist::Value::string("Dashboard Touchscreen"));
    touch.set("displayUUID", plist::Value::string("b7e6c5a0-1111-4000-8000-000000000001"));
    touch.set("hidDescriptor",
              plist::Value::data(multitouchDescriptor(config_.width, config_.height)));
    info.set("hidDevices", plist::Value::array({std::move(touch)}));

    return rtsp::makeResponse(200, "OK", "application/x-apple-binary-plist", plist::encode(info));
}

}  // namespace airplay
