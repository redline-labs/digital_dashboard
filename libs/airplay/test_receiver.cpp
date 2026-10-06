// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RTSP receiver, driven over loopback the way a phone drives it.
//
// Everything here runs before pair-verify, so the control channel is plaintext
// and no phone, MFi part or pairing is needed. That covers what the receiver
// decides on its own: how requests are routed, and what it owns per stream.
#include "airplay/receiver.h"

#include "airplay/rtsp.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

    std::filesystem::remove_all(state_dir);
    if (failures == 0)
    {
        SPDLOG_INFO("all receiver checks passed");
    }
    return failures == 0 ? 0 : 1;
}
