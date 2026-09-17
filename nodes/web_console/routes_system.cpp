// GET /api/system -- what this board is and what it is doing.
//
// Every value comes from libs/system_info, which reads the kernel's own files
// and is tested against a fixture tree. Nothing here parses anything; this
// translates to JSON and decides what a page is allowed to see.

#include "http_server.h"

#include "system_info/system_info.h"

#include <nlohmann/json.hpp>

#include <string>

namespace web_console
{
namespace
{

using nlohmann::json;

// A value the kernel did not give us is null, not zero. The distinction is the
// whole reason system_info returns optionals, and collapsing it here would
// throw that away at the last step.
template <typename T>
json orNull(const std::optional<T>& value)
{
    return value ? json(*value) : json(nullptr);
}

json buildSystemJson()
{
    json out;

    if (const auto release = system_info::readOsRelease())
    {
        out["os"] = {
            {"id", release->id},
            {"name", release->name},
            {"version", release->version},
            {"build_id", release->buildId},
        };
    }
    else
    {
        out["os"] = nullptr;
    }

    if (const auto uptime = system_info::readUptime())
    {
        out["uptime_seconds"] = uptime->count();
    }
    else
    {
        out["uptime_seconds"] = nullptr;
    }

    if (const auto load = system_info::readLoadAverage())
    {
        out["load"] = {load->oneMinute, load->fiveMinutes, load->fifteenMinutes};
    }
    else
    {
        out["load"] = nullptr;
    }

    if (const auto memory = system_info::readMemory())
    {
        out["memory"] = {
            {"total_bytes", orNull(memory->total)},
            {"available_bytes", orNull(memory->available)},
            {"free_bytes", orNull(memory->free)},
            {"buffers_bytes", orNull(memory->buffers)},
            {"cached_bytes", orNull(memory->cached)},
        };
    }
    else
    {
        out["memory"] = nullptr;
    }

    // The three that matter on this image: the running slot, the writable
    // partition, and the ESP the bootloader rewrites.
    json filesystems = json::array();
    for (const char* mount : {"/", "/data", "/boot"})
    {
        if (const auto usage = system_info::readFilesystem(mount))
        {
            filesystems.push_back({
                {"mount_point", usage->mountPoint},
                {"total_bytes", usage->totalBytes},
                {"available_bytes", usage->availableBytes},
            });
        }
    }
    out["filesystems"] = filesystems;

    json interfaces = json::array();
    for (const auto& interface : system_info::readNetworkInterfaces())
    {
        interfaces.push_back({
            {"name", interface.name},
            {"addresses", interface.addresses},
            {"up", interface.up},
            {"loopback", interface.loopback},
        });
    }
    out["network"] = interfaces;

    json temperatures = json::array();
    for (const auto& temperature : system_info::readTemperatures())
    {
        temperatures.push_back({
            {"device", temperature.device},
            {"label", temperature.label},
            {"milli_celsius", temperature.milliCelsius},
        });
    }
    out["temperatures"] = temperatures;

    return out;
}

}  // namespace

void registerSystemRoutes(RouteRegistrar& routes)
{
    routes.get("/api/system", [] { return buildSystemJson().dump(); });
}

}  // namespace web_console
