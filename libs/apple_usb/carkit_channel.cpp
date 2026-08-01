// SPDX-License-Identifier: GPL-3.0-or-later
//
// The carkit channel on our own stack: UsbmuxClient for the transport,
// LockdownClient for the handshake, TlsStream for both TLS sessions. This is the
// replacement for what lockdown.cpp does through libimobiledevice, and the
// dispatcher at the bottom is what chooses between them.
#include "apple_usb/lockdown.h"

#include "apple_usb/lockdown_client.h"
#include "apple_usb/pair_record.h"
#include "apple_usb/tls_stream.h"
#include "apple_usb/usbmux_client.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <optional>
#include <random>
#include <thread>

namespace apple_usb
{

// Defined in lockdown.cpp.
std::unique_ptr<CarkitChannel> openCarkitChannelViaLibimobiledevice(
    const std::string& udid, const std::string& usbmux_socket_path,
    const std::string& pair_record_dir, const std::function<bool()>& abort);

namespace
{

const char* kCarkitService = "com.apple.carkit.service";

// Every device listens here; it is where lockdown itself lives.
constexpr uint16_t kLockdownPort = 62078;

// What the phone shows in Settings > General > VPN & Device Management.
const char* kHostLabel = "mercedes-carplay";

// How long to keep retrying errors that should clear up on their own (the mux
// settling after the configuration switch, a pair record the phone rejected).
// Waiting on the *user* is not bounded by this.
constexpr auto kTransientTimeout = std::chrono::seconds(60);
constexpr auto kHandshakeRetryDelay = std::chrono::milliseconds(1000);

// A first-time pair puts "Trust This Computer?" on the phone and lockdown
// answers PairingDialogResponsePending until the user taps it. There is no
// sensible deadline for that -- the phone may well be in a pocket -- so we wait
// for as long as the node is running and just re-state what we are waiting for
// now and then.
constexpr auto kUserReminderInterval = std::chrono::seconds(30);

// The carkit channel over whatever stream survived the handshake: TLS if the
// service asked for it, the bare mux connection otherwise.
//
// It also holds the LockdownClient that started the service, and that ownership
// is load-bearing rather than tidiness: the device keeps a started service alive
// only for as long as the session that started it. Dropping the lockdown client
// here makes the phone reset the carkit port about a second later, which
// surfaces as "carkit channel died underneath the link layer" right after
// AuthenticationSucceeded.
class NativeCarkitChannel : public CarkitChannel
{
  public:
    NativeCarkitChannel(std::unique_ptr<ByteStream> stream,
                        std::unique_ptr<LockdownClient> lockdown) :
        stream_(std::move(stream)), lockdown_(std::move(lockdown))
    {
    }

    ~NativeCarkitChannel() override { close(); }

    bool send(const uint8_t* data, size_t len) override
    {
        if (stream_ == nullptr || !alive_)
        {
            return false;
        }
        if (!stream_->sendAll(data, len))
        {
            SPDLOG_DEBUG("[carkit] send of {} bytes failed", len);
            alive_ = false;
            return false;
        }
        return true;
    }

    // Gathers until the buffer is full or the timeout expires, rather than
    // returning the first chunk that arrives.
    //
    // This is not a detail, and it is the single change that made the native
    // backend stable: 0 failures in 13 runs with it, 6 in 13 without. The
    // symptom it fixes is the phone resetting the carkit connection about a
    // second after the channel comes up, on the first session of a process.
    //
    // These are libimobiledevice's semantics -- idevice_connection_receive_timeout
    // loops SSL_read until `len` bytes are in hand and returns a partial buffer
    // only once a read times out -- and the iAP2 link layer above was written
    // against them.
    //
    // The likely mechanism is drain rate rather than anything protocol-level.
    // Returning the first chunk hands back as little as a dozen bytes per call,
    // and the link layer runs a whole poll cycle between calls, so a burst (the
    // post-authentication flurry, or a 60 KB album artwork frame) leaves the
    // socket backed up. Our own UsbmuxdServer relay blocks writing into that
    // socket, which stalls the thread pumping the USB mux, which stalls every
    // other stream on it. Draining up to 8 KB per call keeps that from building.
    //
    // Recorded because it was tested and rejected: this is *not* about ACK
    // volume. Runs that died sent 8 ACKs between channel-up and the reset;
    // healthy runs send 11 over the same span. Fewer, not more.
    std::vector<uint8_t> recv(size_t max_len, unsigned timeout_ms) override
    {
        if (stream_ == nullptr || !alive_)
        {
            return {};
        }
        std::vector<uint8_t> buf(max_len);
        size_t received = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (received < max_len)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining =
                now >= deadline
                    ? 0
                    : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

            const ssize_t n =
                stream_->recvSome(buf.data() + received, max_len - received,
                                  static_cast<unsigned>(remaining));
            if (n < 0)
            {
                // Distinguished from a timeout only through alive(); the empty
                // return value looks identical to the caller. Whatever was
                // gathered before the failure is still delivered.
                alive_ = false;
                break;
            }
            if (n == 0)
            {
                break;  // timed out; return what we have, which may be nothing
            }
            received += static_cast<size_t>(n);

            if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
        }

        buf.resize(received);
        return buf;
    }

    bool alive() const override { return alive_; }

    void close() override
    {
        alive_ = false;
        if (stream_ != nullptr)
        {
            stream_->close();
            stream_.reset();
        }
        // Only now is it safe to let the session go.
        lockdown_.reset();
    }

  private:
    std::unique_ptr<ByteStream> stream_;
    std::unique_ptr<LockdownClient> lockdown_;
    bool alive_ = true;
};

// Whether waiting and trying the handshake again can plausibly succeed. The
// pending/locked cases are the normal first-pair path. The pair-record ones are
// not retryable in this loop -- waiting does not fix a record the device has
// rejected -- so they are reported up and answered by re-pairing.
bool waitingOnUser(LockdownError error)
{
    return error == LockdownError::PairingDialogResponsePending ||
           error == LockdownError::PasswordProtected;
}

bool retryable(LockdownError error)
{
    return waitingOnUser(error) || error == LockdownError::Transport;
}

// The device says it does not know this identity. The record is stale -- the
// phone was reset, or trust was revoked -- and only a fresh pairing clears it.
bool recordRejected(LockdownError error)
{
    return error == LockdownError::InvalidHostId || error == LockdownError::InvalidPairRecord ||
           error == LockdownError::MissingPairRecord;
}

// A version-4 UUID, the form lockdown expects for a HostID.
std::string generateHostId()
{
    std::random_device rd;
    std::uniform_int_distribution<int> nibble(0, 15);
    static const char* digits = "0123456789ABCDEF";

    std::string out;
    for (int i = 0; i < 32; ++i)
    {
        if (i == 8 || i == 12 || i == 16 || i == 20)
        {
            out += '-';
        }
        if (i == 12)
        {
            out += '4';  // version
            continue;
        }
        if (i == 16)
        {
            out += digits[8 + (nibble(rd) % 4)];  // variant: 8, 9, A or B
            continue;
        }
        out += digits[nibble(rd)];
    }
    return out;
}

// Pairs a device that has no record, waiting out the trust prompt for as long as
// the node runs. Returns the stored record, or nullopt when the user declined or
// the node is shutting down.
std::optional<PairRecord> pairDevice(UsbmuxClient& mux, const MuxDevice& device,
                                     const std::function<bool()>& abort)
{
    const std::string system_buid = mux.readBuid().value_or("");
    if (system_buid.empty())
    {
        SPDLOG_ERROR("[carkit] no system BUID; cannot pair");
        return std::nullopt;
    }
    // One HostID for the whole attempt, not one per retry: the device remembers
    // the identity from the request that raised the prompt, so a fresh one on
    // each poll would orphan the trust the user just granted.
    const std::string host_id = generateHostId();

    auto next_reminder = std::chrono::steady_clock::time_point::min();
    bool asked = false;

    for (;;)
    {
        if (abort && abort())
        {
            SPDLOG_INFO("[carkit] stopped waiting to pair");
            return std::nullopt;
        }

        auto conn = mux.connect(device.device_id, kLockdownPort);
        if (conn != nullptr)
        {
            const int fd = conn->fd();
            LockdownClient client(std::move(conn), fd, kHostLabel);
            if (client.queryType())
            {
                LockdownError err = LockdownError::Other;
                auto record = client.pair(system_buid, host_id, &err);
                if (record)
                {
                    SPDLOG_INFO("[carkit] paired with udid={}", device.serial.substr(0, 8));
                    if (!mux.savePairRecord(device.serial, record->encode()))
                    {
                        SPDLOG_WARN("[carkit] the new pair record could not be stored; the phone "
                                    "will ask to trust this computer again next time");
                    }
                    return record;
                }

                if (err == LockdownError::UserDeniedPairing)
                {
                    SPDLOG_ERROR("[carkit] the trust prompt was declined on the phone. Unplug "
                                 "it, forget this computer under Settings > General > Transfer "
                                 "or Reset, and retry.");
                    return std::nullopt;
                }
                if (err != LockdownError::PairingDialogResponsePending &&
                    err != LockdownError::PasswordProtected && err != LockdownError::Transport)
                {
                    SPDLOG_ERROR("[carkit] pairing failed: {} ({})", toString(err),
                                 client.lastErrorName());
                    return std::nullopt;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= next_reminder)
                {
                    next_reminder = now + kUserReminderInterval;
                    asked = true;
                    if (err == LockdownError::PasswordProtected)
                    {
                        SPDLOG_WARN("[carkit] the phone is locked; unlock it so it can show the "
                                    "trust prompt; still waiting to pair");
                    }
                    else
                    {
                        SPDLOG_WARN("[carkit] the phone is asking to trust this computer -- tap "
                                    "Trust on it; still waiting to pair");
                    }
                }
            }
        }
        (void)asked;
        std::this_thread::sleep_for(kHandshakeRetryDelay);
    }
}

// Opens a lockdown connection and runs StartSession, waiting out the trust
// prompt. Returns a client with an active session, or nullptr.
std::unique_ptr<LockdownClient> openSession(UsbmuxClient& mux, const MuxDevice& device,
                                            const PairRecord& record,
                                            const std::function<bool()>& abort,
                                            LockdownError* terminal_error)
{
    if (terminal_error != nullptr)
    {
        *terminal_error = LockdownError::None;
    }
    auto transient_deadline = std::chrono::steady_clock::now() + kTransientTimeout;
    auto next_reminder = std::chrono::steady_clock::time_point::min();
    bool waited_on_user = false;

    for (;;)
    {
        if (abort && abort())
        {
            SPDLOG_INFO("[carkit] stopped waiting for the lockdown handshake");
            return nullptr;
        }

        // A fresh connection each attempt: once a StartSession fails the device
        // may have torn the connection down, and TLS may already have been
        // layered onto it either way.
        auto conn = mux.connect(device.device_id, kLockdownPort);
        if (conn == nullptr)
        {
            SPDLOG_DEBUG("[carkit] could not reach the lockdown port");
        }
        else
        {
            const int fd = conn->fd();
            auto client = std::make_unique<LockdownClient>(std::move(conn), fd, kHostLabel);

            std::string type;
            if (!client->queryType(&type))
            {
                SPDLOG_DEBUG("[carkit] QueryType did not answer as lockdown (got '{}')", type);
            }
            else
            {
                const LockdownError err = client->startSession(record);
                if (err == LockdownError::None)
                {
                    if (waited_on_user)
                    {
                        SPDLOG_INFO("[carkit] the phone trusts this computer now");
                    }
                    return client;
                }

                if (!retryable(err))
                {
                    if (terminal_error != nullptr)
                    {
                        *terminal_error = err;
                    }
                    SPDLOG_ERROR("[carkit] lockdown handshake failed: {} ({})", toString(err),
                                 client->lastErrorName());
                    if (err == LockdownError::UserDeniedPairing)
                    {
                        SPDLOG_ERROR("[carkit] the trust prompt was declined on the phone. "
                                     "Unplug it, forget this computer under Settings > General "
                                     "> Transfer or Reset, and retry.");
                    }
                    return nullptr;
                }

                const auto now = std::chrono::steady_clock::now();
                if (waitingOnUser(err))
                {
                    waited_on_user = true;
                    if (now >= next_reminder)
                    {
                        next_reminder = now + kUserReminderInterval;
                        if (err == LockdownError::PairingDialogResponsePending)
                        {
                            SPDLOG_WARN("[carkit] the phone is asking to trust this computer -- "
                                        "tap Trust on it; still waiting");
                        }
                        else
                        {
                            SPDLOG_WARN("[carkit] the phone is locked; unlock it so it can show "
                                        "the trust prompt; still waiting");
                        }
                    }
                    // A slow tap must not eat the budget reserved for transient
                    // faults.
                    transient_deadline = now + kTransientTimeout;
                    std::this_thread::sleep_for(kHandshakeRetryDelay);
                    continue;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= transient_deadline)
        {
            SPDLOG_ERROR("[carkit] gave up after {}s waiting for the lockdown handshake",
                         kTransientTimeout.count());
            return nullptr;
        }
        if (now >= next_reminder)
        {
            next_reminder = now + kUserReminderInterval;
            SPDLOG_WARN("[carkit] lockdown handshake not ready yet; retrying");
        }
        std::this_thread::sleep_for(kHandshakeRetryDelay);
    }
}

std::unique_ptr<CarkitChannel> openNative(const std::string& udid,
                                          const std::string& usbmux_socket_path,
                                          const std::function<bool()>& abort)
{
    UsbmuxClient mux(usbmux_socket_path);

    const auto device = mux.findDevice(udid);
    if (!device)
    {
        SPDLOG_ERROR("[carkit] the mux does not list udid={}", udid.substr(0, 8));
        return nullptr;
    }

    // The pair record comes from the mux rather than from disk: our own
    // UsbmuxdServer owns that store, and going through it keeps one reader.
    std::optional<PairRecord> record;
    if (const auto blob = mux.readPairRecord(device->serial); blob)
    {
        record = PairRecord::parse(*blob);
        if (!record)
        {
            SPDLOG_WARN("[carkit] the stored pair record for udid={} did not parse; pairing again",
                        udid.substr(0, 8));
        }
    }
    if (!record)
    {
        SPDLOG_INFO("[carkit] no usable pair record for udid={}; pairing", udid.substr(0, 8));
        record = pairDevice(mux, *device, abort);
        if (!record)
        {
            return nullptr;
        }
    }

    LockdownError session_error = LockdownError::None;
    auto lockdown = openSession(mux, *device, *record, abort, &session_error);
    if (lockdown == nullptr && recordRejected(session_error))
    {
        // The stored record is no longer one the device recognises. Pair again
        // rather than making someone delete the state directory by hand.
        SPDLOG_WARN("[carkit] the phone rejected our pair record ({}); pairing again",
                    toString(session_error));
        record = pairDevice(mux, *device, abort);
        if (!record)
        {
            return nullptr;
        }
        lockdown = openSession(mux, *device, *record, abort, &session_error);
    }
    if (lockdown == nullptr)
    {
        return nullptr;
    }

    // No escrow bag. It is what lets a service reach data protected while the
    // device is locked, and carkit does not want one: sending it made the phone
    // reset the service connection about a second after the iAP2 link came up,
    // two runs in three. libimobiledevice's lockdownd_start_service passes
    // send_escrow_bag=0 for exactly this call, and matching it makes the link
    // stable.
    const auto service = lockdown->startService(kCarkitService);
    if (!service)
    {
        SPDLOG_ERROR("[carkit] could not start {}: {}", kCarkitService,
                     lockdown->lastErrorName().empty() ? "no port returned"
                                                       : lockdown->lastErrorName());
        return nullptr;
    }
    SPDLOG_DEBUG("[carkit] {} is on port {} (ssl={})", kCarkitService, service->port,
                 service->ssl_enabled);

    // The service lives on its own connection, but the lockdown session has to
    // stay open underneath it -- see NativeCarkitChannel.
    auto conn = mux.connect(device->device_id, service->port);
    if (conn == nullptr)
    {
        SPDLOG_ERROR("[carkit] connect to the carkit port {} failed", service->port);
        return nullptr;
    }

    std::unique_ptr<ByteStream> stream;
    if (service->ssl_enabled)
    {
        const int fd = conn->fd();
        auto tls = TlsStream::connect(std::move(conn), fd, record->root_certificate,
                                      record->root_private_key);
        if (tls == nullptr)
        {
            SPDLOG_ERROR("[carkit] enable SSL on the carkit channel failed");
            return nullptr;
        }
        stream = std::move(tls);
    }
    else
    {
        stream = std::move(conn);
    }

    SPDLOG_INFO("[carkit] carkit TLS channel up (iAP2) udid={}", udid.substr(0, 8));
    return std::make_unique<NativeCarkitChannel>(std::move(stream), std::move(lockdown));
}

}  // namespace

std::unique_ptr<CarkitChannel> openCarkitChannel(const std::string& udid,
                                                 const std::string& usbmux_socket_path,
                                                 const std::string& pair_record_dir,
                                                 std::function<bool()> abort,
                                                 LockdownBackend backend)
{
    if (backend == LockdownBackend::Libimobiledevice)
    {
        SPDLOG_INFO("[carkit] using the libimobiledevice lockdown backend");
        return openCarkitChannelViaLibimobiledevice(udid, usbmux_socket_path, pair_record_dir,
                                                    abort);
    }
    return openNative(udid, usbmux_socket_path, abort);
}

}  // namespace apple_usb
