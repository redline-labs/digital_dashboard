#include "agent_control/control_socket.h"

#include <spdlog/spdlog.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

namespace agent_control
{

namespace
{

bool sendAll(int fd, const char* data, std::size_t len)
{
    std::size_t sent = 0;
    while (sent < len)
    {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

ControlSocket::ControlSocket(LineHandler handler) : handler_(std::move(handler))
{
}

ControlSocket::~ControlSocket()
{
    stop();
}

bool ControlSocket::start(const std::string& socket_path)
{
    if (run_.load())
    {
        SPDLOG_WARN("[agent] control socket already running on {}", socket_path_);
        return false;
    }

    // sockaddr_un::sun_path is a fixed array (104 bytes on macOS, 108 on Linux).
    // A path that does not fit would be silently truncated by strncpy and we
    // would then bind and advertise a socket nobody can find, so reject it here.
    sockaddr_un addr{};
    if (socket_path.size() >= sizeof(addr.sun_path))
    {
        SPDLOG_ERROR("[agent] socket path is {} bytes, max is {}: {}",
                     socket_path.size(), sizeof(addr.sun_path) - 1, socket_path);
        return false;
    }

    ::unlink(socket_path.c_str());

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0)
    {
        SPDLOG_ERROR("[agent] socket() failed: {}", std::strerror(errno));
        return false;
    }

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        SPDLOG_ERROR("[agent] bind({}) failed: {}", socket_path, std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Owner-only. This socket drives the whole UI and can publish onto the
    // vehicle bus, so it is never world-writable the way a debug socket might be.
    if (::chmod(socket_path.c_str(), S_IRUSR | S_IWUSR) < 0)
    {
        SPDLOG_ERROR("[agent] chmod(0600, {}) failed: {}", socket_path, std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(socket_path.c_str());
        return false;
    }

    if (::listen(server_fd_, 4) < 0)
    {
        SPDLOG_ERROR("[agent] listen({}) failed: {}", socket_path, std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(socket_path.c_str());
        return false;
    }

    socket_path_ = socket_path;
    run_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });
    SPDLOG_INFO("[agent] control socket listening on {}", socket_path_);
    return true;
}

void ControlSocket::stop()
{
    if (!run_.exchange(false))
    {
        return;
    }

    // shutdown() before close() so the blocking accept() returns immediately
    // instead of waiting for a connection that will never come.
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

    std::vector<Client> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients.swap(clients_);
    }
    for (auto& client : clients)
    {
        if (client.thread.joinable())
        {
            client.thread.join();
        }
    }

    if (!socket_path_.empty())
    {
        ::unlink(socket_path_.c_str());
    }
    SPDLOG_INFO("[agent] control socket stopped");
}

void ControlSocket::acceptLoop()
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

        reapFinishedClients();

        auto finished = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.push_back(Client{std::thread([this, client, finished]
                                              {
                                                  clientLoop(client);
                                                  finished->store(true);
                                              }),
                                  finished});
    }
}

void ControlSocket::reapFinishedClients()
{
    std::lock_guard<std::mutex> lock(clients_mutex_);

    // A finished thread is still joinable, so completion has to be tracked out
    // of band; join the ones that have signalled and drop them.
    for (auto it = clients_.begin(); it != clients_.end();)
    {
        if (it->finished->load())
        {
            if (it->thread.joinable())
            {
                it->thread.join();
            }
            it = clients_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ControlSocket::clientLoop(int client_fd)
{
    std::string buffer;
    std::array<char, 16384> chunk{};
    bool overlong = false;

    for (;;)
    {
        const ssize_t n = ::recv(client_fd, chunk.data(), chunk.size(), 0);
        if (n <= 0)
        {
            break;
        }

        buffer.append(chunk.data(), static_cast<std::size_t>(n));

        std::size_t newline = buffer.find('\n');
        while (newline != std::string::npos)
        {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);

            // A line that already blew the cap was dropped as it arrived; skip
            // its tail rather than handing a truncated fragment to the parser.
            if (overlong)
            {
                overlong = false;
            }
            else if (!line.empty())
            {
                std::string response = handler_(line);
                if (!response.empty())
                {
                    response.push_back('\n');
                    if (!sendAll(client_fd, response.data(), response.size()))
                    {
                        ::close(client_fd);
                        return;
                    }
                }
            }

            newline = buffer.find('\n');
        }

        if (buffer.size() > kMaxLineBytes)
        {
            SPDLOG_WARN("[agent] dropping oversized request (> {} bytes)", kMaxLineBytes);
            buffer.clear();
            overlong = true;
        }
    }

    ::close(client_fd);
}

}  // namespace agent_control
