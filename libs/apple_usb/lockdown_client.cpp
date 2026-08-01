// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_usb/lockdown_client.h"

#include "apple_usb/tls_stream.h"
#include "plist/binary.h"
#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>

namespace apple_usb
{

namespace
{

// Lockdown frames a plist with a four-byte big-endian length. Requests go out as
// XML; replies come back as either, so they are sniffed.
constexpr size_t kLengthPrefixBytes = 4;

// A lockdown reply is a handful of kilobytes at most. The length comes off the
// wire before anything is allocated for it.
constexpr uint32_t kMaxMessageBytes = 1u << 22;

// The device answers within a second or so on a healthy link. The long one is
// for StartSession, which can sit behind the trust prompt on the device side
// before it answers at all.
constexpr unsigned kDefaultTimeoutMs = 10000;

constexpr const char* kProtocolVersion = "2";

uint32_t readBe32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void writeBe32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(value & 0xFF);
}

// Reads exactly n bytes, or fails. Anything short of that leaves the stream
// framed mid-message, so there is no partial success to report.
bool readExact(ByteStream& stream, uint8_t* out, size_t n, unsigned timeout_ms)
{
    size_t got = 0;
    while (got < n)
    {
        const ssize_t r = stream.recvSome(out + got, n - got, timeout_ms);
        if (r < 0)
        {
            return false;
        }
        if (r == 0)
        {
            SPDLOG_DEBUG("[lockdown] timed out with {}/{} bytes of a message", got, n);
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

struct ErrorName
{
    const char* name;
    LockdownError error;
};

// The subset that changes what a caller does. Everything else lands on Other and
// is still visible through lastErrorName().
constexpr std::array<ErrorName, 6> kErrorNames = {{
    {"PairingDialogResponsePending", LockdownError::PairingDialogResponsePending},
    {"PasswordProtected", LockdownError::PasswordProtected},
    {"InvalidHostID", LockdownError::InvalidHostId},
    {"InvalidPairRecord", LockdownError::InvalidPairRecord},
    {"MissingPairRecord", LockdownError::MissingPairRecord},
    {"UserDeniedPairing", LockdownError::UserDeniedPairing},
}};

}  // namespace

const char* toString(LockdownError error)
{
    switch (error)
    {
        case LockdownError::None: return "success";
        case LockdownError::PairingDialogResponsePending: return "waiting for the trust prompt";
        case LockdownError::PasswordProtected: return "the device is locked";
        case LockdownError::InvalidHostId: return "the device rejected our host id";
        case LockdownError::InvalidPairRecord: return "the device rejected our pair record";
        case LockdownError::MissingPairRecord: return "no pair record for this device";
        case LockdownError::UserDeniedPairing: return "the trust prompt was declined";
        case LockdownError::Transport: return "the lockdown connection failed";
        case LockdownError::Other: return "lockdown reported an error";
    }
    return "unknown";
}

LockdownClient::LockdownClient(std::unique_ptr<ByteStream> stream, int fd, std::string label) :
    stream_(std::move(stream)), fd_(fd), label_(std::move(label))
{
}

LockdownClient::~LockdownClient()
{
    // StopSession on the way out, which is what libimobiledevice's
    // lockdownd_client_free does. Leaving the session dangling makes the device
    // reap it on its own schedule, and it does so around a second into the
    // *next* connection -- which looks like the new session dying for no reason.
    //
    // This is only safe because a service started under the session outlives it
    // no better: whoever holds the service has to hold this client too, and does
    // (see NativeCarkitChannel).
    stopSession();
    if (stream_ != nullptr)
    {
        stream_->close();
    }
}

plist::Value LockdownClient::requestDict(const char* request) const
{
    plist::Value d = plist::Value::dict();
    d.set("Label", plist::Value::string(label_));
    d.set("ProtocolVersion", plist::Value::string(kProtocolVersion));
    d.set("Request", plist::Value::string(request));
    return d;
}

bool LockdownClient::sendPlist(const plist::Value& value)
{
    if (stream_ == nullptr)
    {
        return false;
    }
    const std::string xml = plist::encodeXml(value);
    uint8_t header[kLengthPrefixBytes];
    writeBe32(header, static_cast<uint32_t>(xml.size()));

    return stream_->sendAll(header, sizeof(header)) &&
           stream_->sendAll(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
}

std::optional<plist::Value> LockdownClient::receivePlist(unsigned timeout_ms)
{
    if (stream_ == nullptr)
    {
        return std::nullopt;
    }
    uint8_t header[kLengthPrefixBytes];
    if (!readExact(*stream_, header, sizeof(header), timeout_ms))
    {
        return std::nullopt;
    }
    const uint32_t length = readBe32(header);
    if (length == 0 || length > kMaxMessageBytes)
    {
        SPDLOG_DEBUG("[lockdown] message length {} is out of range", length);
        return std::nullopt;
    }

    std::vector<uint8_t> body(length);
    if (!readExact(*stream_, body.data(), body.size(), timeout_ms))
    {
        return std::nullopt;
    }

    if (plist::looksBinary(body))
    {
        return plist::decodeBinary(body);
    }
    return plist::decodeXml(
        std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
}

std::optional<plist::Value> LockdownClient::transact(const plist::Value& request)
{
    if (!sendPlist(request))
    {
        SPDLOG_DEBUG("[lockdown] send failed");
        return std::nullopt;
    }
    return receivePlist(kDefaultTimeoutMs);
}

LockdownError LockdownClient::checkResult(const plist::Value& reply, const char* expected_request)
{
    last_error_name_.clear();

    if (const plist::Value* error = reply.find("Error"); error != nullptr && error->isString())
    {
        last_error_name_ = error->asString();
        for (const ErrorName& candidate : kErrorNames)
        {
            if (last_error_name_ == candidate.name)
            {
                return candidate.error;
            }
        }
        return LockdownError::Other;
    }

    // A well-formed success echoes the request name back. Not every reply does,
    // so its absence is not treated as a failure -- only a mismatch is.
    if (const plist::Value* request = reply.find("Request");
        request != nullptr && request->isString() && request->asString() != expected_request)
    {
        SPDLOG_DEBUG("[lockdown] reply is for {} but we asked for {}", request->asString(),
                     expected_request);
        last_error_name_ = "RequestMismatch";
        return LockdownError::Other;
    }
    return LockdownError::None;
}

bool LockdownClient::queryType(std::string* type_out)
{
    const auto reply = transact(requestDict("QueryType"));
    if (!reply)
    {
        return false;
    }
    if (checkResult(*reply, "QueryType") != LockdownError::None)
    {
        return false;
    }
    const plist::Value* type = reply->find("Type");
    if (type == nullptr || !type->isString())
    {
        return false;
    }
    if (type_out != nullptr)
    {
        *type_out = type->asString();
    }
    return type->asString() == "com.apple.mobile.lockdown";
}

std::optional<plist::Value> LockdownClient::getValue(const std::string& domain,
                                                     const std::string& key)
{
    plist::Value request = requestDict("GetValue");
    if (!domain.empty())
    {
        request.set("Domain", plist::Value::string(domain));
    }
    if (!key.empty())
    {
        request.set("Key", plist::Value::string(key));
    }

    const auto reply = transact(request);
    if (!reply || checkResult(*reply, "GetValue") != LockdownError::None)
    {
        return std::nullopt;
    }
    const plist::Value* value = reply->find("Value");
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return *value;
}

std::optional<PairRecord> LockdownClient::pair(const std::string& system_buid,
                                               const std::string& host_id,
                                               LockdownError* error_out)
{
    const auto fail = [&](LockdownError error) -> std::optional<PairRecord> {
        if (error_out != nullptr)
        {
            *error_out = error;
        }
        return std::nullopt;
    };

    const auto public_key = getValue("", "DevicePublicKey");
    if (!public_key || !public_key->isData())
    {
        SPDLOG_DEBUG("[lockdown] the device would not give up its public key");
        return fail(LockdownError::Transport);
    }

    // Read the WiFi address before pairing rather than after. libimobiledevice
    // notes that asking afterwards makes iOS 7 drop the connection, and the
    // value is only wanted for the stored record either way.
    std::string wifi_mac;
    if (const auto wifi = getValue("", "WiFiAddress"); wifi && wifi->isString())
    {
        wifi_mac = wifi->asString();
    }

    auto record = PairRecord::generate(public_key->asData(), system_buid, host_id);
    if (!record)
    {
        return fail(LockdownError::Other);
    }
    record->wifi_mac_address = wifi_mac;

    plist::Value request = requestDict("Pair");
    // The certificates and identifiers only. The private keys are ours and never
    // go on the wire.
    plist::Value sent = plist::Value::dict();
    sent.set("DeviceCertificate", plist::Value::data(record->device_certificate));
    sent.set("HostCertificate", plist::Value::data(record->host_certificate));
    sent.set("RootCertificate", plist::Value::data(record->root_certificate));
    sent.set("SystemBUID", plist::Value::string(record->system_buid));
    sent.set("HostID", plist::Value::string(record->host_id));
    request.set("PairRecord", std::move(sent));
    // Without this the device collapses several distinct refusals into a single
    // generic one, and "the user has not answered yet" stops being separable
    // from "the user said no".
    plist::Value options = plist::Value::dict();
    options.set("ExtendedPairingErrors", plist::Value::boolean(true));
    request.set("PairingOptions", std::move(options));

    const auto reply = transact(request);
    if (!reply)
    {
        return fail(LockdownError::Transport);
    }

    const LockdownError result = checkResult(*reply, "Pair");
    if (result != LockdownError::None)
    {
        return fail(result);
    }

    // The device returns the escrow bag on a successful pair, and it belongs in
    // the stored record -- it is what later lets a service reach data protected
    // while the phone is locked.
    if (const plist::Value* bag = reply->find("EscrowBag"); bag != nullptr && bag->isData())
    {
        record->escrow_bag = bag->asData();
    }

    if (error_out != nullptr)
    {
        *error_out = LockdownError::None;
    }
    return record;
}

LockdownError LockdownClient::startSession(const PairRecord& record)
{
    if (record.host_id.empty())
    {
        return LockdownError::MissingPairRecord;
    }

    plist::Value request = requestDict("StartSession");
    request.set("HostID", plist::Value::string(record.host_id));
    if (!record.system_buid.empty())
    {
        // A record is only valid for the BUID it was made under. Sending it lets
        // the device say InvalidHostID rather than silently failing later.
        request.set("SystemBUID", plist::Value::string(record.system_buid));
    }

    const auto reply = transact(request);
    if (!reply)
    {
        return LockdownError::Transport;
    }

    const LockdownError result = checkResult(*reply, "StartSession");
    if (result != LockdownError::None)
    {
        return result;
    }

    if (const plist::Value* id = reply->find("SessionID"); id != nullptr && id->isString())
    {
        session_id_ = id->asString();
    }

    bool enable_ssl = false;
    if (const plist::Value* ssl = reply->find("EnableSessionSSL");
        ssl != nullptr && ssl->isBool())
    {
        enable_ssl = ssl->asBool();
    }
    if (!enable_ssl)
    {
        // Every device in scope asks for TLS. If one does not, the session is
        // still usable, just unencrypted.
        SPDLOG_DEBUG("[lockdown] session started without SSL");
        return LockdownError::None;
    }

    auto tls = TlsStream::connect(std::move(stream_), fd_, record.root_certificate,
                                  record.root_private_key);
    if (tls == nullptr)
    {
        SPDLOG_ERROR("[lockdown] could not enable TLS on the session");
        // stream_ was consumed by the failed attempt; the client is finished.
        session_id_.clear();
        return LockdownError::Transport;
    }
    stream_ = std::move(tls);
    SPDLOG_DEBUG("[lockdown] session {} up with TLS", session_id_);
    return LockdownError::None;
}

std::optional<LockdownService> LockdownClient::startService(
    const std::string& identifier, const std::vector<uint8_t>& escrow_bag)
{
    plist::Value request = requestDict("StartService");
    request.set("Service", plist::Value::string(identifier));
    if (!escrow_bag.empty())
    {
        request.set("EscrowBag", plist::Value::data(escrow_bag));
    }

    const auto reply = transact(request);
    if (!reply)
    {
        return std::nullopt;
    }
    if (checkResult(*reply, "StartService") != LockdownError::None)
    {
        SPDLOG_DEBUG("[lockdown] StartService({}) failed: {}", identifier, last_error_name_);
        return std::nullopt;
    }

    LockdownService service;
    if (const plist::Value* port = reply->find("Port"); port != nullptr)
    {
        service.port = static_cast<uint16_t>(port->asInteger());
    }
    if (const plist::Value* ssl = reply->find("EnableServiceSSL");
        ssl != nullptr && ssl->isBool())
    {
        service.ssl_enabled = ssl->asBool();
    }
    if (service.port == 0)
    {
        SPDLOG_DEBUG("[lockdown] StartService({}) returned no port", identifier);
        return std::nullopt;
    }
    return service;
}

void LockdownClient::stopSession()
{
    if (session_id_.empty() || stream_ == nullptr)
    {
        return;
    }
    plist::Value request = requestDict("StopSession");
    request.set("SessionID", plist::Value::string(session_id_));
    // Best effort: the session is being torn down either way.
    (void)transact(request);
    session_id_.clear();
}

}  // namespace apple_usb
