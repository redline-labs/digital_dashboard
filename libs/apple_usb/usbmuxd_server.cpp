// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py (UsbmuxdServer)
#include "apple_usb/usbmuxd_server.h"

#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>

namespace fs = std::filesystem;

namespace apple_usb
{

namespace
{

// usbmuxd packet header (little-endian): length(incl header), version=1(plist),
// message=8(plist), tag.
constexpr uint32_t kPlistVersion = 1;
constexpr uint32_t kPlistMessage = 8;

// A sane ceiling on a control message. The largest thing a client legitimately
// sends is a SavePairRecord carrying a few kilobytes of PEM; the length field is
// attacker-controlled, so it is bounded before it becomes an allocation.
constexpr uint32_t kMaxRequestBytes = 1u << 20;

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

// A single client request: the header's tag, echoed back on the reply, and the
// parsed body.
struct Request
{
    uint32_t tag = 0;
    plist::Value body;
};

// Reads one plist request. Returns nullopt on EOF, a malformed header, or a body
// that is not a plist -- all of which mean the same thing here (drop the client).
std::optional<Request> recvPacket(int fd)
{
    uint8_t hdr[16];
    if (!recvExact(fd, hdr, sizeof(hdr)))
    {
        return std::nullopt;
    }
    uint32_t length = 0;
    Request request;
    std::memcpy(&length, hdr + 0, 4);
    std::memcpy(&request.tag, hdr + 12, 4);
    if (length < 16 || length > kMaxRequestBytes)
    {
        return std::nullopt;
    }
    std::vector<uint8_t> body(length - 16);
    if (!body.empty() && !recvExact(fd, body.data(), body.size()))
    {
        return std::nullopt;
    }
    auto parsed = plist::decodeXml(
        std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
    if (!parsed)
    {
        return std::nullopt;
    }
    request.body = std::move(*parsed);
    return request;
}

void sendReply(int fd, uint32_t tag, const plist::Value& dict)
{
    const std::string xml = plist::encodeXml(dict);

    const std::array<uint32_t, 4> hdr = {static_cast<uint32_t>(16 + xml.size()), kPlistVersion,
                                         kPlistMessage, tag};
    ::send(fd, hdr.data(), sizeof(hdr), MSG_NOSIGNAL);
    ::send(fd, xml.data(), xml.size(), MSG_NOSIGNAL);
}

plist::Value resultDict(int number)
{
    plist::Value d = plist::Value::dict();
    d.set("MessageType", plist::Value::string("Result"));
    d.set("Number", plist::Value::integer(number));
    return d;
}

std::string dictString(const plist::Value& dict, const char* key)
{
    const plist::Value* node = dict.find(key);
    if (node == nullptr || !node->isString())
    {
        return {};
    }
    return node->asString();
}

// The dashed spelling of an undashed 24-character serial, and vice versa.
// Empty when the input is neither form.
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

std::string genUuid()
{
    std::random_device rd;
    std::uniform_int_distribution<int> hex(0, 15);
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 32; ++i)
    {
        if (i == 8 || i == 12 || i == 16 || i == 20)
        {
            s += '-';
        }
        s += digits[hex(rd)];
    }
    return s;
}

}  // namespace

UsbmuxdServer::UsbmuxdServer(MuxHost& host, std::string socket_path, std::string state_dir) :
    host_(host), socket_path_(std::move(socket_path)), state_dir_(std::move(state_dir))
{
}

UsbmuxdServer::~UsbmuxdServer()
{
    stop();
}

bool UsbmuxdServer::start()
{
    ::unlink(socket_path_.c_str());
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0)
    {
        SPDLOG_ERROR("[usbmuxd] socket() failed: {}", strerror(errno));
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        SPDLOG_ERROR("[usbmuxd] bind({}) failed: {}", socket_path_, strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    ::chmod(socket_path_.c_str(), 0777);
    ::listen(server_fd_, 16);

    run_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });
    SPDLOG_INFO("[usbmuxd] serving {} on {}", host_.serial().substr(0, 8), socket_path_);
    return true;
}

void UsbmuxdServer::stop()
{
    run_.store(false);
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
    for (auto& t : client_threads_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    client_threads_.clear();
    ::unlink(socket_path_.c_str());
}

void UsbmuxdServer::acceptLoop()
{
    while (run_.load())
    {
        const int client = ::accept(server_fd_, nullptr, nullptr);
        if (client < 0)
        {
            if (run_.load())
            {
                continue;
            }
            return;
        }
        client_threads_.emplace_back([this, client] { clientLoop(client); });
    }
}

void UsbmuxdServer::clientLoop(int client_fd)
{
    for (;;)
    {
        const auto request = recvPacket(client_fd);
        if (!request)
        {
            break;
        }
        const plist::Value& req = request->body;
        const uint32_t tag = request->tag;

        const std::string mt = dictString(req, "MessageType");
        if (mt == "ReadBUID")
        {
            plist::Value d = plist::Value::dict();
            d.set("BUID", plist::Value::string(readBuid()));
            sendReply(client_fd, tag, d);
        }
        else if (mt == "ListDevices")
        {
            plist::Value list = plist::Value::array();
            list.push(deviceEntry());
            plist::Value d = plist::Value::dict();
            d.set("DeviceList", std::move(list));
            sendReply(client_fd, tag, d);
        }
        else if (mt == "Listen")
        {
            sendReply(client_fd, tag, resultDict(0));
        }
        else if (mt == "ReadPairRecord")
        {
            std::string id = dictString(req, "PairRecordID");
            if (id.empty()) id = host_.serial();
            const auto rec = readPairRecord(id);
            if (rec.empty())
            {
                sendReply(client_fd, tag, resultDict(2));  // ENOENT
            }
            else
            {
                plist::Value d = plist::Value::dict();
                d.set("PairRecordData", plist::Value::data(rec));
                sendReply(client_fd, tag, d);
            }
        }
        else if (mt == "SavePairRecord")
        {
            std::string id = dictString(req, "PairRecordID");
            if (id.empty()) id = host_.serial();
            if (const plist::Value* data_node = req.find("PairRecordData");
                data_node != nullptr && data_node->isData())
            {
                const auto& buf = data_node->asData();
                savePairRecord(id, buf.data(), buf.size());
            }
            sendReply(client_fd, tag, resultDict(0));
        }
        else if (mt == "Connect")
        {
            uint16_t port = 0;
            if (const plist::Value* pn = req.find("PortNumber"); pn != nullptr)
            {
                // usbmux carries the port in network order.
                port = ntohs(static_cast<uint16_t>(pn->asInteger()));
            }

            auto conn = host_.connect(port);
            if (!conn)
            {
                sendReply(client_fd, tag, resultDict(3));  // connection refused
                break;
            }
            sendReply(client_fd, tag, resultDict(0));
            relay(client_fd, conn);  // takes over the socket until EOF
            ::close(client_fd);
            return;
        }
        else
        {
            sendReply(client_fd, tag, resultDict(0));
        }
    }
    ::close(client_fd);
}

void UsbmuxdServer::relay(int client_fd, std::shared_ptr<MuxTcpConn> conn)
{
    // Device -> client pump on a helper thread.
    std::thread up([client_fd, conn] {
        while (!conn->closed())
        {
            std::vector<uint8_t> data = conn->recv();
            if (data.empty())
            {
                break;
            }
            if (::send(client_fd, data.data(), data.size(), MSG_NOSIGNAL) < 0)
            {
                break;
            }
        }
        ::shutdown(client_fd, SHUT_RDWR);
    });

    // Client -> device pump on this thread.
    std::vector<uint8_t> buf(16384);
    for (;;)
    {
        const ssize_t r = ::recv(client_fd, buf.data(), buf.size(), 0);
        if (r <= 0)
        {
            break;
        }
        conn->send(buf.data(), static_cast<size_t>(r));
    }
    conn->close();
    up.join();
}

// --- pair-record / BUID store (mirrors muxd.py, keyed under state_dir) ---

std::string UsbmuxdServer::readBuid()
{
    const fs::path p = fs::path(state_dir_) / "SystemConfiguration.plist";
    std::error_code ec;
    if (fs::exists(p, ec))
    {
        std::ifstream in(p, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (const auto root = plist::decodeXml(content); root)
        {
            const std::string buid = dictString(*root, "SystemBUID");
            if (!buid.empty())
            {
                return buid;
            }
        }
    }

    // A pair record is only valid for the SystemBUID it was created under, so a
    // BUID minted fresh on every run would invalidate the record we just saved
    // and re-prompt for trust on the phone at every start. Persist it.
    const std::string buid = genUuid();
    plist::Value root = plist::Value::dict();
    root.set("SystemBUID", plist::Value::string(buid));
    const std::string xml = plist::encodeXml(root);

    fs::create_directories(state_dir_, ec);
    std::ofstream out(p, std::ios::binary);
    out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    if (!out.good())
    {
        SPDLOG_WARN("[usbmuxd] could not persist the system BUID to {}; the phone will ask to "
                    "trust this computer again on the next run", p.string());
    }
    return buid;
}

std::vector<uint8_t> UsbmuxdServer::readPairRecord(const std::string& udid)
{
    // libusbmuxd normalises a 24-character serial into the dashed 25-character
    // form, so the ID a client asks for need not be the one a past run saved
    // under. Accept either spelling.
    for (const auto& name : {udid, alternateUdidForm(udid)})
    {
        if (name.empty())
        {
            continue;
        }
        const fs::path p = fs::path(state_dir_) / (name + ".plist");
        std::error_code ec;
        if (fs::exists(p, ec))
        {
            std::ifstream in(p, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

void UsbmuxdServer::savePairRecord(const std::string& udid, const uint8_t* data, size_t len)
{
    std::error_code ec;
    fs::create_directories(state_dir_, ec);
    std::ofstream out(fs::path(state_dir_) / (udid + ".plist"), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

plist::Value UsbmuxdServer::deviceEntry()
{
    plist::Value props = plist::Value::dict();
    props.set("ConnectionType", plist::Value::string("USB"));
    props.set("SerialNumber", plist::Value::string(host_.serial()));
    props.set("DeviceID", plist::Value::integer(1));
    props.set("LocationID", plist::Value::integer(0));
    props.set("ProductID", plist::Value::integer(0x12a8));

    plist::Value entry = plist::Value::dict();
    entry.set("DeviceID", plist::Value::integer(1));
    entry.set("MessageType", plist::Value::string("Attached"));
    entry.set("Properties", std::move(props));
    return entry;
}

}  // namespace apple_usb
