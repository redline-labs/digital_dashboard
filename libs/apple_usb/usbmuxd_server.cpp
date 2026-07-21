// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py (UsbmuxdServer)
#include "apple_usb/usbmuxd_server.h"

#include <spdlog/spdlog.h>
#include <plist/plist.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// Reads one plist request; returns tag and the parsed dict (caller frees).
bool recvPacket(int fd, uint32_t& tag, plist_t& out)
{
    uint8_t hdr[16];
    if (!recvExact(fd, hdr, sizeof(hdr)))
    {
        return false;
    }
    uint32_t length;
    std::memcpy(&length, hdr + 0, 4);
    std::memcpy(&tag, hdr + 12, 4);
    if (length < 16)
    {
        return false;
    }
    std::vector<uint8_t> body(length - 16);
    if (!body.empty() && !recvExact(fd, body.data(), body.size()))
    {
        return false;
    }
    out = nullptr;
    plist_from_xml(reinterpret_cast<const char*>(body.data()), static_cast<uint32_t>(body.size()), &out);
    return out != nullptr;
}

void sendReply(int fd, uint32_t tag, plist_t dict)
{
    char* xml = nullptr;
    uint32_t xml_len = 0;
    plist_to_xml(dict, &xml, &xml_len);

    std::array<uint32_t, 4> hdr = {16 + xml_len, kPlistVersion, kPlistMessage, tag};
    ::send(fd, hdr.data(), sizeof(hdr), MSG_NOSIGNAL);
    ::send(fd, xml, xml_len, MSG_NOSIGNAL);
    plist_mem_free(xml);
}

plist_t resultDict(int number)
{
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "MessageType", plist_new_string("Result"));
    plist_dict_set_item(d, "Number", plist_new_uint(static_cast<uint64_t>(number)));
    return d;
}

std::string dictString(plist_t dict, const char* key)
{
    plist_t node = plist_dict_get_item(dict, key);
    if (node == nullptr || plist_get_node_type(node) != PLIST_STRING)
    {
        return {};
    }
    char* val = nullptr;
    plist_get_string_val(node, &val);
    std::string out = val ? val : "";
    plist_mem_free(val);
    return out;
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
        uint32_t tag = 0;
        plist_t req = nullptr;
        if (!recvPacket(client_fd, tag, req))
        {
            break;
        }

        const std::string mt = dictString(req, "MessageType");
        if (mt == "ReadBUID")
        {
            plist_t d = plist_new_dict();
            plist_dict_set_item(d, "BUID", plist_new_string(readBuid().c_str()));
            sendReply(client_fd, tag, d);
            plist_free(d);
        }
        else if (mt == "ListDevices")
        {
            plist_t list = plist_new_array();
            plist_array_append_item(list, deviceEntry());
            plist_t d = plist_new_dict();
            plist_dict_set_item(d, "DeviceList", list);
            sendReply(client_fd, tag, d);
            plist_free(d);
        }
        else if (mt == "Listen")
        {
            plist_t d = resultDict(0);
            sendReply(client_fd, tag, d);
            plist_free(d);
        }
        else if (mt == "ReadPairRecord")
        {
            std::string id = dictString(req, "PairRecordID");
            if (id.empty()) id = host_.serial();
            const auto rec = readPairRecord(id);
            if (rec.empty())
            {
                plist_t d = resultDict(2);  // ENOENT
                sendReply(client_fd, tag, d);
                plist_free(d);
            }
            else
            {
                plist_t d = plist_new_dict();
                plist_dict_set_item(d, "PairRecordData",
                                    plist_new_data(reinterpret_cast<const char*>(rec.data()), rec.size()));
                sendReply(client_fd, tag, d);
                plist_free(d);
            }
        }
        else if (mt == "SavePairRecord")
        {
            std::string id = dictString(req, "PairRecordID");
            if (id.empty()) id = host_.serial();
            plist_t data_node = plist_dict_get_item(req, "PairRecordData");
            if (data_node != nullptr && plist_get_node_type(data_node) == PLIST_DATA)
            {
                char* buf = nullptr;
                uint64_t buf_len = 0;
                plist_get_data_val(data_node, &buf, &buf_len);
                savePairRecord(id, reinterpret_cast<const uint8_t*>(buf), buf_len);
                plist_mem_free(buf);
            }
            plist_t d = resultDict(0);
            sendReply(client_fd, tag, d);
            plist_free(d);
        }
        else if (mt == "Connect")
        {
            uint16_t port = 0;
            if (plist_t pn = plist_dict_get_item(req, "PortNumber"); pn != nullptr)
            {
                uint64_t v = 0;
                plist_get_uint_val(pn, &v);
                port = ntohs(static_cast<uint16_t>(v));  // usbmux carries the port in network order
            }
            plist_free(req);

            auto conn = host_.connect(port);
            if (!conn)
            {
                plist_t d = resultDict(3);  // connection refused
                sendReply(client_fd, tag, d);
                plist_free(d);
                break;
            }
            plist_t d = resultDict(0);
            sendReply(client_fd, tag, d);
            plist_free(d);
            relay(client_fd, conn);  // takes over the socket until EOF
            ::close(client_fd);
            return;
        }
        else
        {
            plist_t d = resultDict(0);
            sendReply(client_fd, tag, d);
            plist_free(d);
        }

        plist_free(req);
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
        plist_t root = nullptr;
        plist_from_xml(content.c_str(), static_cast<uint32_t>(content.size()), &root);
        if (root != nullptr)
        {
            const std::string buid = dictString(root, "SystemBUID");
            plist_free(root);
            if (!buid.empty())
            {
                return buid;
            }
        }
    }
    return genUuid();
}

std::vector<uint8_t> UsbmuxdServer::readPairRecord(const std::string& udid)
{
    for (const auto& name : {udid, udid})
    {
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

plist_t UsbmuxdServer::deviceEntry()
{
    plist_t props = plist_new_dict();
    plist_dict_set_item(props, "ConnectionType", plist_new_string("USB"));
    plist_dict_set_item(props, "SerialNumber", plist_new_string(host_.serial().c_str()));
    plist_dict_set_item(props, "DeviceID", plist_new_uint(1));
    plist_dict_set_item(props, "LocationID", plist_new_uint(0));
    plist_dict_set_item(props, "ProductID", plist_new_uint(0x12a8));

    plist_t entry = plist_new_dict();
    plist_dict_set_item(entry, "DeviceID", plist_new_uint(1));
    plist_dict_set_item(entry, "MessageType", plist_new_string("Attached"));
    plist_dict_set_item(entry, "Properties", props);
    return entry;
}

}  // namespace apple_usb
