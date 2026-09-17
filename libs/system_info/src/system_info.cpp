#include "system_info/system_info.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace system_info
{
namespace
{

namespace fs = std::filesystem;

std::optional<std::string> slurp(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

// os-release quotes values that contain spaces and leaves the rest bare, so a
// reader that does not strip is correct about half the time -- which is the
// half that shows up in a screenshot.
std::string unquote(std::string_view text)
{
    std::string value = trim(text);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
    {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::optional<double> toDouble(const std::string& text)
{
    try
    {
        return std::stod(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<std::uint64_t> toUnsigned(const std::string& text)
{
    try
    {
        return static_cast<std::uint64_t>(std::stoull(text));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

}  // namespace

std::optional<OsRelease> readOsRelease(const Roots& roots)
{
    const std::optional<std::string> contents = slurp(roots.etc / "os-release");
    if (!contents)
    {
        return std::nullopt;
    }

    OsRelease release;
    std::istringstream lines(*contents);
    std::string line;
    std::string prettyName;
    std::string versionId;
    while (std::getline(lines, line))
    {
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.starts_with("#"))
        {
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, equals));
        const std::string value = unquote(std::string_view(line).substr(equals + 1));

        if (key == "ID") { release.id = value; }
        else if (key == "NAME") { release.name = value; }
        else if (key == "PRETTY_NAME") { prettyName = value; }
        else if (key == "VERSION") { release.version = value; }
        else if (key == "VERSION_ID") { versionId = value; }
        else if (key == "BUILD_ID") { release.buildId = value; }
    }

    // PRETTY_NAME is what a person recognises; NAME is the fallback, not the
    // preference. Same for VERSION over the bare VERSION_ID.
    if (!prettyName.empty()) { release.name = prettyName; }
    if (release.version.empty()) { release.version = versionId; }
    return release;
}

std::optional<std::chrono::seconds> readUptime(const Roots& roots)
{
    const std::optional<std::string> contents = slurp(roots.proc / "uptime");
    if (!contents)
    {
        return std::nullopt;
    }
    std::istringstream fields(*contents);
    std::string first;
    if (!(fields >> first))
    {
        return std::nullopt;
    }
    const std::optional<double> seconds = toDouble(first);
    if (!seconds || *seconds < 0.0)
    {
        return std::nullopt;
    }
    return std::chrono::seconds(static_cast<std::int64_t>(*seconds));
}

std::optional<LoadAverage> readLoadAverage(const Roots& roots)
{
    const std::optional<std::string> contents = slurp(roots.proc / "loadavg");
    if (!contents)
    {
        return std::nullopt;
    }
    std::istringstream fields(*contents);
    std::string one;
    std::string five;
    std::string fifteen;
    if (!(fields >> one >> five >> fifteen))
    {
        return std::nullopt;
    }
    const auto a = toDouble(one);
    const auto b = toDouble(five);
    const auto c = toDouble(fifteen);
    if (!a || !b || !c)
    {
        return std::nullopt;
    }
    return LoadAverage{.oneMinute = *a, .fiveMinutes = *b, .fifteenMinutes = *c};
}

std::optional<Memory> readMemory(const Roots& roots)
{
    const std::optional<std::string> contents = slurp(roots.proc / "meminfo");
    if (!contents)
    {
        return std::nullopt;
    }

    Memory memory;
    std::istringstream lines(*contents);
    std::string line;
    while (std::getline(lines, line))
    {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, colon));
        std::istringstream rest(line.substr(colon + 1));
        std::string number;
        std::string unit;
        rest >> number >> unit;

        std::optional<std::uint64_t> value = toUnsigned(number);
        if (!value)
        {
            continue;
        }
        // meminfo is kB everywhere except a handful of count fields, which we
        // do not read. The unit belongs here rather than in every caller.
        if (unit == "kB")
        {
            *value *= 1024u;
        }

        if (key == "MemTotal") { memory.total = value; }
        else if (key == "MemAvailable") { memory.available = value; }
        else if (key == "MemFree") { memory.free = value; }
        else if (key == "Buffers") { memory.buffers = value; }
        else if (key == "Cached") { memory.cached = value; }
    }

    if (!memory.total)
    {
        return std::nullopt;  // No MemTotal means this was not meminfo.
    }
    return memory;
}

std::optional<FilesystemUsage> readFilesystem(const fs::path& mountPoint)
{
    struct statvfs stats{};
    if (::statvfs(mountPoint.c_str(), &stats) != 0)
    {
        return std::nullopt;
    }

    // f_frsize, not f_bsize: the former is the fragment size the block counts
    // are actually in. They are equal on every filesystem we ship, which is
    // exactly why using the wrong one survives testing.
    const std::uint64_t unit = stats.f_frsize != 0 ? stats.f_frsize : stats.f_bsize;
    return FilesystemUsage{
        .mountPoint = mountPoint.string(),
        .totalBytes = static_cast<std::uint64_t>(stats.f_blocks) * unit,
        // f_bavail, not f_bfree: the difference is the root reserve, which a
        // non-root writer cannot have.
        .availableBytes = static_cast<std::uint64_t>(stats.f_bavail) * unit,
    };
}

std::vector<NetworkInterface> readNetworkInterfaces()
{
    std::vector<NetworkInterface> interfaces;

    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0)
    {
        return interfaces;
    }

    for (const ifaddrs* entry = head; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr)
        {
            continue;
        }
        const int family = entry->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
        {
            continue;
        }

        char text[INET6_ADDRSTRLEN] = {};
        const void* address = nullptr;
        if (family == AF_INET)
        {
            address = &reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr;
        }
        else
        {
            address = &reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr)->sin6_addr;
        }
        if (::inet_ntop(family, address, text, sizeof(text)) == nullptr)
        {
            continue;
        }

        const std::string name = entry->ifa_name != nullptr ? entry->ifa_name : "";
        auto existing = std::find_if(interfaces.begin(), interfaces.end(),
                                     [&name](const NetworkInterface& i) { return i.name == name; });
        if (existing == interfaces.end())
        {
            interfaces.push_back(NetworkInterface{
                .name = name,
                .addresses = {},
                .up = (entry->ifa_flags & IFF_UP) != 0,
                .loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0,
            });
            existing = std::prev(interfaces.end());
        }
        existing->addresses.emplace_back(text);
    }

    ::freeifaddrs(head);
    return interfaces;
}

std::vector<Temperature> readTemperatures(const Roots& roots)
{
    std::vector<Temperature> temperatures;

    const fs::path hwmonRoot = roots.sys / "class/hwmon";
    std::error_code ec;
    if (!fs::is_directory(hwmonRoot, ec))
    {
        return temperatures;
    }

    // directory_iterator order is unspecified, and a landing page that lists
    // sensors in a different order on every read looks like it is flickering.
    std::vector<fs::path> devices;
    for (const auto& entry : fs::directory_iterator(hwmonRoot, ec))
    {
        devices.push_back(entry.path());
    }
    std::sort(devices.begin(), devices.end());

    for (const fs::path& device : devices)
    {
        const std::string deviceName = trim(slurp(device / "name").value_or(""));

        std::vector<fs::path> inputs;
        for (const auto& entry : fs::directory_iterator(device, ec))
        {
            const std::string file = entry.path().filename().string();
            if (file.starts_with("temp") && file.ends_with("_input"))
            {
                inputs.push_back(entry.path());
            }
        }
        std::sort(inputs.begin(), inputs.end());

        for (const fs::path& input : inputs)
        {
            const std::optional<std::string> raw = slurp(input);
            if (!raw)
            {
                continue;
            }
            const std::string trimmed = trim(*raw);
            std::int32_t milli = 0;
            try
            {
                milli = static_cast<std::int32_t>(std::stol(trimmed));
            }
            catch (const std::exception&)
            {
                continue;  // A sensor that reports nothing readable is not a zero.
            }

            const std::string stem = input.filename().string().substr(0, input.filename().string().size() - 6);
            const std::string label = trim(slurp(device / (stem + "_label")).value_or(stem));

            temperatures.push_back(Temperature{
                .device = deviceName,
                .label = label,
                .milliCelsius = milli,
            });
        }
    }

    return temperatures;
}

}  // namespace system_info
