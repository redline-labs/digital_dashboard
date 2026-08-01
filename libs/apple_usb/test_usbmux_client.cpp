// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drives UsbmuxClient against a mock server that speaks the usbmux wire format
// directly. The real UsbmuxdServer cannot be used here because it needs a
// MuxHost, which needs a phone on a usbfs node; the mock keeps the framing,
// message shapes and error paths testable on any machine.
//
// The counterpart hardware check -- our client against our real server over a
// real socket -- is the apple_usb_muxctl tool.
#include "apple_usb/usbmux_client.h"

#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

constexpr uint32_t kHeaderBytes = 16;

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

// A minimal usbmux server: one client at a time, answering from a fixed script.
class MockMux
{
  public:
    explicit MockMux(std::string path) : path_(std::move(path))
    {
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path_.c_str(), path_.size());
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            SPDLOG_ERROR("mock bind failed: {}", std::strerror(errno));
        }
        ::listen(fd_, 8);
        thread_ = std::thread([this] { serve(); });
    }

    ~MockMux()
    {
        run_.store(false);
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        if (thread_.joinable())
        {
            thread_.join();
        }
        ::unlink(path_.c_str());
    }

    const std::string& path() const { return path_; }

    // Set by the Connect handler so a test can confirm what was asked for.
    std::atomic<uint16_t> last_connect_port{0};
    std::atomic<uint32_t> last_connect_device{0};
    // Makes Connect answer "refused" instead of succeeding.
    std::atomic<bool> refuse_connect{false};
    // The bytes the relay writes back after a successful Connect.
    std::string relay_greeting = "HELLO-FROM-DEVICE";

  private:
    void serve()
    {
        while (run_.load())
        {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0)
            {
                return;
            }
            handle(client);
            ::close(client);
        }
    }

    static void reply(int fd, uint32_t tag, const plist::Value& value)
    {
        const std::string xml = plist::encodeXml(value);
        const uint32_t hdr[4] = {static_cast<uint32_t>(kHeaderBytes + xml.size()), 1, 8, tag};
        ::send(fd, hdr, sizeof(hdr), MSG_NOSIGNAL);
        ::send(fd, xml.data(), xml.size(), MSG_NOSIGNAL);
    }

    static plist::Value resultDict(int number)
    {
        plist::Value d = plist::Value::dict();
        d.set("MessageType", plist::Value::string("Result"));
        d.set("Number", plist::Value::integer(number));
        return d;
    }

    void handle(int client)
    {
        uint8_t hdr[kHeaderBytes];
        if (!recvExact(client, hdr, sizeof(hdr)))
        {
            return;
        }
        uint32_t length = 0;
        uint32_t tag = 0;
        std::memcpy(&length, hdr + 0, 4);
        std::memcpy(&tag, hdr + 12, 4);
        std::vector<uint8_t> body(length - kHeaderBytes);
        if (!body.empty() && !recvExact(client, body.data(), body.size()))
        {
            return;
        }
        const auto request = plist::decodeXml(
            std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
        if (!request)
        {
            return;
        }

        const plist::Value* type = request->find("MessageType");
        const std::string mt = (type != nullptr && type->isString()) ? type->asString() : "";

        if (mt == "ReadBUID")
        {
            plist::Value d = plist::Value::dict();
            d.set("BUID", plist::Value::string("EA1C45CE-C8F1-DE62-8ACF-82637111A89D"));
            reply(client, tag, d);
        }
        else if (mt == "ListDevices")
        {
            plist::Value props = plist::Value::dict();
            props.set("ConnectionType", plist::Value::string("USB"));
            props.set("SerialNumber", plist::Value::string("00008140-000138EE0184801C"));
            props.set("DeviceID", plist::Value::integer(1));
            props.set("ProductID", plist::Value::integer(0x12a8));

            plist::Value entry = plist::Value::dict();
            entry.set("DeviceID", plist::Value::integer(1));
            entry.set("MessageType", plist::Value::string("Attached"));
            entry.set("Properties", std::move(props));

            plist::Value list = plist::Value::array();
            list.push(std::move(entry));
            plist::Value d = plist::Value::dict();
            d.set("DeviceList", std::move(list));
            reply(client, tag, d);
        }
        else if (mt == "ReadPairRecord")
        {
            const plist::Value* id = request->find("PairRecordID");
            const std::string want = (id != nullptr && id->isString()) ? id->asString() : "";
            if (want == "known")
            {
                plist::Value d = plist::Value::dict();
                const std::string pem = "-----BEGIN CERTIFICATE-----\nabc\n";
                d.set("PairRecordData",
                      plist::Value::data(std::vector<uint8_t>(pem.begin(), pem.end())));
                reply(client, tag, d);
            }
            else
            {
                reply(client, tag, resultDict(2));  // ENOENT
            }
        }
        else if (mt == "SavePairRecord")
        {
            const plist::Value* data = request->find("PairRecordData");
            saved_ok_ = data != nullptr && data->isData() && data->asData().size() == 4096;
            reply(client, tag, resultDict(saved_ok_ ? 0 : 1));
        }
        else if (mt == "Connect")
        {
            if (const plist::Value* pn = request->find("PortNumber"); pn != nullptr)
            {
                last_connect_port.store(ntohs(static_cast<uint16_t>(pn->asInteger())));
            }
            if (const plist::Value* did = request->find("DeviceID"); did != nullptr)
            {
                last_connect_device.store(static_cast<uint32_t>(did->asInteger()));
            }
            if (refuse_connect.load())
            {
                reply(client, tag, resultDict(3));
                return;
            }
            reply(client, tag, resultDict(0));
            // The socket is now a raw pipe. Say something, then echo.
            ::send(client, relay_greeting.data(), relay_greeting.size(), MSG_NOSIGNAL);
            std::vector<uint8_t> buf(256);
            for (;;)
            {
                const ssize_t n = ::recv(client, buf.data(), buf.size(), 0);
                if (n <= 0)
                {
                    return;
                }
                ::send(client, buf.data(), static_cast<size_t>(n), MSG_NOSIGNAL);
            }
        }
        else
        {
            reply(client, tag, resultDict(0));
        }
    }

    std::string path_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> run_{true};
    bool saved_ok_ = false;
};

void testControlMessages(apple_usb::UsbmuxClient& client)
{
    const auto buid = client.readBuid();
    expect(buid.has_value() && *buid == "EA1C45CE-C8F1-DE62-8ACF-82637111A89D", "ReadBUID");

    const auto devices = client.listDevices();
    expect(devices.size() == 1, "ListDevices returns one device");
    if (devices.size() == 1)
    {
        expect(devices[0].device_id == 1, "device id");
        expect(devices[0].serial == "00008140-000138EE0184801C", "serial");
        expect(devices[0].connection_type == "USB", "connection type");
        expect(devices[0].product_id == 0x12a8, "product id");
    }
}

// The mux reports the dashed spelling; sysfs gives the undashed one. Both must
// find the device, which is the bug the old normalizeUdid() worked around.
void testUdidForms(apple_usb::UsbmuxClient& client)
{
    expect(client.findDevice("00008140-000138EE0184801C").has_value(), "dashed UDID matches");
    expect(client.findDevice("00008140000138EE0184801C").has_value(), "undashed UDID matches");
    expect(!client.findDevice("00000000000000000000000A").has_value(), "unknown UDID does not");
}

void testPairRecords(apple_usb::UsbmuxClient& client)
{
    const auto known = client.readPairRecord("known");
    expect(known.has_value() && !known->empty(), "ReadPairRecord returns a known record");

    const auto missing = client.readPairRecord("unknown");
    expect(!missing.has_value(), "ReadPairRecord reports a missing record as absent");

    // 4096 bytes is what the mock checks for, standing in for a real record.
    expect(client.savePairRecord("known", std::vector<uint8_t>(4096, 0x5A)),
           "SavePairRecord round-trips a realistic blob");
}

void testConnectAndRelay(MockMux& mock, apple_usb::UsbmuxClient& client)
{
    auto conn = client.connect(1, 62078);
    expect(conn != nullptr, "Connect to the lockdown port succeeds");
    if (conn == nullptr)
    {
        return;
    }
    expect(mock.last_connect_port.load() == 62078, "port arrives in network order");
    expect(mock.last_connect_device.load() == 1, "device id arrives");

    // The greeting proves the socket became a byte pipe.
    std::vector<uint8_t> buf(64);
    const ssize_t n = conn->recvSome(buf.data(), buf.size(), 2000);
    expect(n > 0, "the connection carries device bytes");
    if (n > 0)
    {
        expect(std::string(buf.begin(), buf.begin() + n) == mock.relay_greeting,
               "greeting arrives intact");
    }

    const std::string payload = "ping";
    expect(conn->sendAll(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
           "the connection accepts a write");
    const ssize_t echoed = conn->recvSome(buf.data(), buf.size(), 2000);
    expect(echoed == static_cast<ssize_t>(payload.size()), "the echo comes back");
    if (echoed > 0)
    {
        expect(std::string(buf.begin(), buf.begin() + echoed) == payload, "echo matches");
    }

    // A timeout is 0, distinct from the -1 that means the peer is gone.
    std::vector<uint8_t> idle(16);
    expect(conn->recvSome(idle.data(), idle.size(), 100) == 0, "an idle read times out as 0");

    conn->close();
    expect(conn->recvSome(idle.data(), idle.size(), 100) == -1, "a closed read reports -1");
}

void testConnectRefused(MockMux& mock, apple_usb::UsbmuxClient& client)
{
    mock.refuse_connect.store(true);
    expect(client.connect(1, 1234) == nullptr, "a refused Connect returns nullptr");
    mock.refuse_connect.store(false);
}

void testNoServer()
{
    apple_usb::UsbmuxClient orphan("/tmp/definitely-not-a-usbmux-socket-12345");
    expect(!orphan.readBuid().has_value(), "ReadBUID fails with no server");
    expect(orphan.listDevices().empty(), "ListDevices is empty with no server");
    expect(orphan.connect(1, 62078) == nullptr, "Connect fails with no server");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    const std::string path =
        (fs::temp_directory_path() / ("usbmux-test-" + std::to_string(::getpid()) + ".sock"))
            .string();

    {
        MockMux mock(path);
        apple_usb::UsbmuxClient client(path);

        testControlMessages(client);
        testUdidForms(client);
        testPairRecords(client);
        testConnectAndRelay(mock, client);
        testConnectRefused(mock, client);
    }

    testNoServer();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} usbmux client assertion(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all usbmux client tests passed");
    return 0;
}
