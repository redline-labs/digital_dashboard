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

    std::vector<uint8_t> recv(size_t max_len, unsigned timeout_ms) override
    {
        if (stream_ == nullptr || !alive_)
        {
            return {};
        }
        std::vector<uint8_t> buf(max_len);
        const ssize_t n = stream_->recvSome(buf.data(), max_len, timeout_ms);
        if (n < 0)
        {
            // Distinguished from a timeout only through alive(); the empty
            // return value looks identical to the caller.
            alive_ = false;
            return {};
        }
        buf.resize(static_cast<size_t>(n));
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
// pending/locked cases are the normal first-pair path; the pair-record ones are
// retryable only in the sense that re-pairing would fix them, which this backend
// cannot do -- they are reported and given up on.
bool waitingOnUser(LockdownError error)
{
    return error == LockdownError::PairingDialogResponsePending ||
           error == LockdownError::PasswordProtected;
}

bool retryable(LockdownError error)
{
    return waitingOnUser(error) || error == LockdownError::Transport;
}

// Opens a lockdown connection and runs StartSession, waiting out the trust
// prompt. Returns a client with an active session, or nullptr.
std::unique_ptr<LockdownClient> openSession(UsbmuxClient& mux, const MuxDevice& device,
                                            const PairRecord& record,
                                            const std::function<bool()>& abort)
{
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
                    SPDLOG_ERROR("[carkit] lockdown handshake failed: {} ({})", toString(err),
                                 client->lastErrorName());
                    if (err == LockdownError::UserDeniedPairing)
                    {
                        SPDLOG_ERROR("[carkit] the trust prompt was declined on the phone. "
                                     "Unplug it, forget this computer under Settings > General "
                                     "> Transfer or Reset, and retry.");
                    }
                    if (err == LockdownError::InvalidHostId ||
                        err == LockdownError::InvalidPairRecord ||
                        err == LockdownError::MissingPairRecord)
                    {
                        SPDLOG_ERROR("[carkit] this backend cannot re-pair. Delete the state dir "
                                     "and run once with --lockdown-backend libimobiledevice.");
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
    const auto blob = mux.readPairRecord(device->serial);
    if (!blob)
    {
        SPDLOG_ERROR("[carkit] no pair record for udid={}. This backend can use a pair record "
                     "but cannot create one -- run once with --lockdown-backend "
                     "libimobiledevice to pair, then this backend will work.",
                     udid.substr(0, 8));
        return nullptr;
    }
    const auto record = PairRecord::parse(*blob);
    if (!record)
    {
        SPDLOG_ERROR("[carkit] the pair record for udid={} did not parse", udid.substr(0, 8));
        return nullptr;
    }

    auto lockdown = openSession(mux, *device, *record, abort);
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
