#include "core/core.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef REDLINE_SOURCE_DIR
#define REDLINE_SOURCE_DIR ""
#endif

namespace core
{

namespace
{

constexpr const char* kPattern = "[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v";

std::optional<std::string> env(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
}

}  // namespace

// ---- logging ----------------------------------------------------------------

void setupLogging(bool debug_enabled)
{
    spdlog::set_pattern(kPattern);
    spdlog::set_level(debug_enabled ? spdlog::level::debug : spdlog::level::info);
}

void setupLogging(const LoggingOptions& options)
{
    auto& sinks = spdlog::default_logger()->sinks();

    if (options.stderr_only)
    {
        sinks.clear();
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }
    else if (const auto dir = paths::logDir())
    {
        std::error_code ec;
        std::filesystem::create_directories(*dir, ec);
        const std::string file =
            *dir + "/" + (options.program.empty() ? std::string("log") : options.program) + ".txt";
        try
        {
            constexpr size_t max_size_bytes = 5u * 1024u * 1024u;
            constexpr size_t max_files = 3u;
            sinks.push_back(
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, max_size_bytes, max_files, true));
        }
        catch (const spdlog::spdlog_ex& e)
        {
            // A log file that cannot be opened must never take the program
            // down; say so on the console and carry on with it alone.
            SPDLOG_WARN("REDLINE_LOG_DIR={}: cannot open {}: {}", *dir, file, e.what());
        }
    }

    setupLogging(options.debug);
}

// ---- paths ------------------------------------------------------------------

namespace paths
{

std::optional<std::string> logDir()
{
    return env("REDLINE_LOG_DIR");
}

std::string dataDir()
{
    if (const auto dir = env("REDLINE_DATA_DIR"))
    {
        return *dir;
    }
    const std::string home = env("HOME").value_or(".");
#ifdef __APPLE__
    return home + "/Library/Application Support/redline";
#else
    if (const auto xdg = env("XDG_DATA_HOME"))
    {
        return *xdg + "/redline";
    }
    return home + "/.local/share/redline";
#endif
}

std::string executableDir()
{
    std::string path;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0)
    {
        path = buffer.c_str();
    }
#else
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0)
    {
        buffer[n] = '\0';
        path = buffer;
    }
#endif
    if (path.empty())
    {
        return "";
    }
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return (ec ? std::filesystem::path(path) : canonical).parent_path().string();
}

std::string resource(std::string_view relative)
{
    std::error_code ec;
    const std::string exe_dir = executableDir();
    if (!exe_dir.empty())
    {
        const auto installed = std::filesystem::path(exe_dir) / ".." / std::string(relative);
        if (std::filesystem::exists(installed, ec))
        {
            return std::filesystem::weakly_canonical(installed, ec).string();
        }
    }
    const std::string source = REDLINE_SOURCE_DIR;
    if (!source.empty())
    {
        const auto in_tree = std::filesystem::path(source) / std::string(relative);
        if (std::filesystem::exists(in_tree, ec))
        {
            return in_tree.string();
        }
    }
    return std::string(relative);
}

std::string expand(std::string_view value, std::string_view relative_to)
{
    std::string out;
    out.reserve(value.size());

    for (size_t i = 0; i < value.size();)
    {
        if (value[i] == '$' && i + 1 < value.size() && value[i + 1] == '{')
        {
            const size_t close = value.find('}', i + 2);
            if (close != std::string_view::npos)
            {
                const std::string name(value.substr(i + 2, close - i - 2));
                if (name == "REDLINE_DATA_DIR")
                {
                    out += dataDir();
                }
                else if (const auto v = env(name.c_str()))
                {
                    out += *v;
                }
                i = close + 1;
                continue;
            }
        }
        out += value[i];
        ++i;
    }

    if (out.size() >= 2 && out[0] == '~' && out[1] == '/')
    {
        out = env("HOME").value_or(".") + out.substr(1);
    }

    if (!out.empty() && out[0] != '/' && !relative_to.empty())
    {
        out = (std::filesystem::path(std::string(relative_to)) / out).lexically_normal().string();
    }
    return out;
}

}  // namespace paths

// ---- systemd ----------------------------------------------------------------

namespace systemd
{

namespace
{

bool notify(std::string_view message)
{
    const auto socket_path = env("NOTIFY_SOCKET");
    if (!socket_path)
    {
        return false;
    }
    // sd_notify(3): a filesystem path, or an abstract socket when it starts
    // with '@'.
    const std::string& path = *socket_path;
    if (path.size() >= sizeof(sockaddr_un::sun_path))
    {
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());
    if (addr.sun_path[0] == '@')
    {
        addr.sun_path[0] = '\0';
    }
    else
    {
        len += 1;
    }

    const ssize_t sent = ::sendto(fd, message.data(), message.size(), 0,
                                  reinterpret_cast<const sockaddr*>(&addr), len);
    ::close(fd);
    return sent == static_cast<ssize_t>(message.size());
}

}  // namespace

bool notifyReady()
{
    return notify("READY=1\n");
}

bool notifyStatus(std::string_view status)
{
    return notify("STATUS=" + std::string(status) + "\n");
}

}  // namespace systemd

}  // namespace core
