// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_usb/usbmux_client.h"

#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstring>

namespace apple_usb
{

namespace
{

// Mirrors the header UsbmuxdServer writes: little-endian length (including the
// header), version=1 (plist), message=8 (plist), tag.
constexpr uint32_t kPlistVersion = 1;
constexpr uint32_t kPlistMessage = 8;
constexpr uint32_t kHeaderBytes = 16;

// The reply to ListDevices grows with the device list but stays far under this.
// The length field comes off a socket, so it is bounded before it is trusted.
constexpr uint32_t kMaxReplyBytes = 1u << 20;

// Identifies us in the handshake. usbmuxd logs these and nothing branches on
// them, but omitting them is a visible deviation from every other client.
constexpr const char* kClientVersion = "digital_dashboard usbmux client";
constexpr const char* kProgName = "digital_dashboard";
constexpr const char* kBundleId = "com.github.digital_dashboard";

bool sendAllBytes(int fd, const void* data, size_t len)
{
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len)
    {
        const ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvExact(int fd, void* buf, size_t n)
{
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n)
    {
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r <= 0)
        {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

int connectUnix(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        SPDLOG_ERROR("[usbmux] socket() failed: {}", std::strerror(errno));
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        SPDLOG_ERROR("[usbmux] socket path is too long: {}", path);
        ::close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        SPDLOG_DEBUG("[usbmux] connect({}) failed: {}", path, std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

// Adds the fields every usbmux request carries, so callers only state what is
// specific to their message.
plist::Value requestDict(const char* message_type)
{
    plist::Value d = plist::Value::dict();
    d.set("BundleID", plist::Value::string(kBundleId));
    d.set("ClientVersionString", plist::Value::string(kClientVersion));
    d.set("MessageType", plist::Value::string(message_type));
    d.set("ProgName", plist::Value::string(kProgName));
    d.set("kLibUSBMuxVersion", plist::Value::integer(3));
    return d;
}

bool writeRequest(int fd, uint32_t tag, const plist::Value& request)
{
    const std::string xml = plist::encodeXml(request);
    const std::array<uint32_t, 4> hdr = {static_cast<uint32_t>(kHeaderBytes + xml.size()),
                                         kPlistVersion, kPlistMessage, tag};
    return sendAllBytes(fd, hdr.data(), sizeof(hdr)) && sendAllBytes(fd, xml.data(), xml.size());
}

std::optional<plist::Value> readReply(int fd, uint32_t expected_tag)
{
    uint8_t hdr[kHeaderBytes];
    if (!recvExact(fd, hdr, sizeof(hdr)))
    {
        return std::nullopt;
    }
    uint32_t length = 0;
    uint32_t tag = 0;
    std::memcpy(&length, hdr + 0, 4);
    std::memcpy(&tag, hdr + 12, 4);
    if (length < kHeaderBytes || length > kMaxReplyBytes)
    {
        SPDLOG_DEBUG("[usbmux] reply length {} is out of range", length);
        return std::nullopt;
    }
    if (tag != expected_tag)
    {
        // Requests are issued one per connection, so a mismatched tag means the
        // stream is out of step rather than that a reply arrived out of order.
        SPDLOG_DEBUG("[usbmux] reply tag {} does not match request tag {}", tag, expected_tag);
        return std::nullopt;
    }
    std::vector<uint8_t> body(length - kHeaderBytes);
    if (!body.empty() && !recvExact(fd, body.data(), body.size()))
    {
        return std::nullopt;
    }
    return plist::decodeXml(
        std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
}

// The "Number" out of a Result reply, or nullopt when the reply is not one.
// Zero means success; the rest are errno-ish values usbmuxd defines.
std::optional<int64_t> resultNumber(const plist::Value& reply)
{
    const plist::Value* type = reply.find("MessageType");
    if (type == nullptr || !type->isString() || type->asString() != "Result")
    {
        return std::nullopt;
    }
    const plist::Value* number = reply.find("Number");
    if (number == nullptr)
    {
        return std::nullopt;
    }
    return number->asInteger();
}

// One request/reply exchange on a fresh connection. `keep_open` hands the socket
// back instead of closing it, which is what Connect needs.
//
// Not named `exchange`: the first argument is a std::string, so ADL would pull
// std::exchange into the overload set and it wins.
std::optional<plist::Value> sendAndReceive(const std::string& socket_path,
                                           const plist::Value& request, int* keep_open = nullptr)
{
    const int fd = connectUnix(socket_path);
    if (fd < 0)
    {
        return std::nullopt;
    }
    // Any constant would do -- one request lives on each connection -- but a
    // varying tag makes a desynchronised stream obvious rather than accidentally
    // self-consistent.
    static uint32_t next_tag = 1;
    const uint32_t tag = next_tag++;

    if (!writeRequest(fd, tag, request))
    {
        ::close(fd);
        return std::nullopt;
    }
    auto reply = readReply(fd, tag);
    if (!reply || keep_open == nullptr)
    {
        ::close(fd);
        return reply;
    }
    *keep_open = fd;
    return reply;
}

// The dashed spelling of an undashed 24-character serial, and vice versa. The
// two forms name the same device and different sources report different ones.
std::string alternateUdidForm(const std::string& udid)
{
    if (udid.size() == 24 && udid.find('-') == std::string::npos)
    {
        return udid.substr(0, 8) + "-" + udid.substr(8);
    }
    if (udid.size() == 25 && udid[8] == '-')
    {
        return udid.substr(0, 8) + udid.substr(9);
    }
    return {};
}

}  // namespace

// --- MuxConnection ----------------------------------------------------------

MuxConnection::MuxConnection(int fd) : fd_(fd) {}

MuxConnection::~MuxConnection()
{
    close();
}

void MuxConnection::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MuxConnection::sendAll(const uint8_t* data, size_t len)
{
    return fd_ >= 0 && sendAllBytes(fd_, data, len);
}

ssize_t MuxConnection::recvSome(uint8_t* out, size_t max_len, unsigned timeout_ms)
{
    if (fd_ < 0)
    {
        return -1;
    }
    pollfd pfd{fd_, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (ready == 0)
    {
        return 0;
    }
    if (ready < 0)
    {
        // A signal during the wait is not a failure of the connection.
        return errno == EINTR ? 0 : -1;
    }
    const ssize_t n = ::recv(fd_, out, max_len, 0);
    if (n == 0)
    {
        return -1;  // orderly shutdown by the peer
    }
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
    return n;
}

// --- UsbmuxClient -----------------------------------------------------------

UsbmuxClient::UsbmuxClient(std::string socket_path) : socket_path_(std::move(socket_path)) {}

std::optional<std::string> UsbmuxClient::readBuid()
{
    const auto reply = sendAndReceive(socket_path_, requestDict("ReadBUID"));
    if (!reply)
    {
        return std::nullopt;
    }
    const plist::Value* buid = reply->find("BUID");
    if (buid == nullptr || !buid->isString())
    {
        SPDLOG_DEBUG("[usbmux] ReadBUID reply carried no BUID");
        return std::nullopt;
    }
    return buid->asString();
}

std::vector<MuxDevice> UsbmuxClient::listDevices()
{
    std::vector<MuxDevice> out;
    const auto reply = sendAndReceive(socket_path_, requestDict("ListDevices"));
    if (!reply)
    {
        return out;
    }
    const plist::Value* list = reply->find("DeviceList");
    if (list == nullptr || !list->isArray())
    {
        SPDLOG_DEBUG("[usbmux] ListDevices reply carried no DeviceList");
        return out;
    }

    for (size_t i = 0; i < list->size(); ++i)
    {
        const plist::Value& entry = list->at(i);
        const plist::Value* props = entry.find("Properties");
        if (props == nullptr || !props->isDict())
        {
            continue;
        }
        MuxDevice device;
        if (const plist::Value* id = entry.find("DeviceID"); id != nullptr)
        {
            device.device_id = static_cast<uint32_t>(id->asInteger());
        }
        if (const plist::Value* serial = props->find("SerialNumber");
            serial != nullptr && serial->isString())
        {
            device.serial = serial->asString();
        }
        if (const plist::Value* type = props->find("ConnectionType");
            type != nullptr && type->isString())
        {
            device.connection_type = type->asString();
        }
        if (const plist::Value* pid = props->find("ProductID"); pid != nullptr)
        {
            device.product_id = static_cast<uint32_t>(pid->asInteger());
        }
        out.push_back(std::move(device));
    }
    return out;
}

std::optional<MuxDevice> UsbmuxClient::findDevice(const std::string& udid)
{
    const std::string alternate = alternateUdidForm(udid);
    for (const MuxDevice& device : listDevices())
    {
        if (device.serial == udid || (!alternate.empty() && device.serial == alternate))
        {
            return device;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> UsbmuxClient::readPairRecord(const std::string& record_id)
{
    plist::Value request = requestDict("ReadPairRecord");
    request.set("PairRecordID", plist::Value::string(record_id));

    const auto reply = sendAndReceive(socket_path_, request);
    if (!reply)
    {
        return std::nullopt;
    }
    const plist::Value* data = reply->find("PairRecordData");
    if (data == nullptr || !data->isData())
    {
        // A Result reply here means "no record", which is a normal first-run
        // state rather than a failure.
        return std::nullopt;
    }
    return data->asData();
}

bool UsbmuxClient::savePairRecord(const std::string& record_id, const std::vector<uint8_t>& data)
{
    plist::Value request = requestDict("SavePairRecord");
    request.set("PairRecordID", plist::Value::string(record_id));
    request.set("PairRecordData", plist::Value::data(data));

    const auto reply = sendAndReceive(socket_path_, request);
    if (!reply)
    {
        return false;
    }
    const auto number = resultNumber(*reply);
    return number.has_value() && *number == 0;
}

std::unique_ptr<MuxConnection> UsbmuxClient::connect(uint32_t device_id, uint16_t port)
{
    plist::Value request = requestDict("Connect");
    request.set("DeviceID", plist::Value::integer(device_id));
    // usbmux carries the port in network byte order inside a host-order integer.
    request.set("PortNumber", plist::Value::integer(htons(port)));

    int fd = -1;
    const auto reply = sendAndReceive(socket_path_, request, &fd);
    if (!reply)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
        return nullptr;
    }

    const auto number = resultNumber(*reply);
    if (!number.has_value() || *number != 0)
    {
        SPDLOG_DEBUG("[usbmux] Connect to port {} refused (result {})", port,
                     number.has_value() ? *number : -1);
        ::close(fd);
        return nullptr;
    }
    // From here the socket is a raw pipe to the device port.
    return std::make_unique<MuxConnection>(fd);
}

}  // namespace apple_usb
