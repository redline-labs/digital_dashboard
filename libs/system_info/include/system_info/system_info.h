#ifndef SYSTEM_INFO_SYSTEM_INFO_H_
#define SYSTEM_INFO_SYSTEM_INFO_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// What this board is and what it is doing, read from the kernel's own files.
//
// The web console's landing page answers questions a person asks at a bench:
// which build is this, how long has it been up, how much memory is left, is the
// network up, how warm is it. Every one of those is a file under /proc, /sys or
// /etc, so this library reads files and parses them, and does nothing else --
// no HTTP, no bus, no formatting. Wiring it to a socket is nodes/web_console.
//
// EVERY READER TAKES ITS ROOT. That is the whole reason this is a library
// rather than a handful of helpers in the node: a parser pointed at a fixture
// directory is testable on any machine, and /proc is not something a test can
// arrange. The defaults are the real paths, so callers on the target say
// nothing; the tests say everything.
//
// Values are optional rather than zero. A field the kernel did not give us and
// a field that is genuinely zero are different answers, and a landing page that
// renders "0" for "could not read" is worse than one that says nothing.

namespace system_info
{

// Where the readers look. Overridden wholesale by tests, never in production.
struct Roots
{
    std::filesystem::path proc { "/proc" };
    std::filesystem::path sys { "/sys" };
    std::filesystem::path etc { "/etc" };
};

// /etc/os-release. Keys are unquoted; a file that is not there yields nullopt
// rather than an empty identity, because "no os-release" is a broken image and
// worth showing as such.
struct OsRelease
{
    std::string id;           // ID=
    std::string name;         // NAME= or PRETTY_NAME=
    std::string version;      // VERSION= / VERSION_ID=
    std::string buildId;      // BUILD_ID=, which is what a Yocto image stamps
};
std::optional<OsRelease> readOsRelease(const Roots& roots = {});

// /proc/uptime. The first field; the second (idle) is per-CPU-summed and not
// useful here.
std::optional<std::chrono::seconds> readUptime(const Roots& roots = {});

// /proc/loadavg, the three decayed averages.
struct LoadAverage
{
    double oneMinute { 0.0 };
    double fiveMinutes { 0.0 };
    double fifteenMinutes { 0.0 };
};
std::optional<LoadAverage> readLoadAverage(const Roots& roots = {});

// /proc/meminfo, in BYTES rather than the file's kB -- the unit belongs at the
// boundary, not in every caller. MemAvailable is the one to show: it is the
// kernel's own estimate of what a new allocation can have, which MemFree is not.
struct Memory
{
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> available;
    std::optional<std::uint64_t> free;
    std::optional<std::uint64_t> buffers;
    std::optional<std::uint64_t> cached;
};
std::optional<Memory> readMemory(const Roots& roots = {});

// A mounted filesystem's capacity, from statvfs rather than a file, so this one
// cannot be fixtured and is tested against paths that exist everywhere.
struct FilesystemUsage
{
    std::string mountPoint;
    std::uint64_t totalBytes { 0 };
    std::uint64_t availableBytes { 0 };
};
std::optional<FilesystemUsage> readFilesystem(const std::filesystem::path& mountPoint);

// The interfaces with addresses, from getifaddrs(3). Loopback is included and
// flagged rather than dropped: "only lo is up" is a diagnosis, and a page that
// silently omits it looks the same as a page with no network at all.
struct NetworkInterface
{
    std::string name;
    std::vector<std::string> addresses;  // presentation form, v4 and v6
    bool up { false };
    bool loopback { false };
};
std::vector<NetworkInterface> readNetworkInterfaces();

// Every temp*_input under every hwmon device, in millidegrees as the kernel
// reports them. Deliberately NOT a reimplementation of
// display_backlight::readTemperatures(), which reads ONE module's hwmon from a
// display record; this walks the whole class for a board-wide view. A caller
// wanting the panel's sensors should ask that one.
struct Temperature
{
    std::string device;   // hwmon name attribute, e.g. "coretemp"
    std::string label;    // temp<n>_label if present, else "temp<n>"
    std::int32_t milliCelsius { 0 };
};
std::vector<Temperature> readTemperatures(const Roots& roots = {});

} // namespace system_info

#endif // SYSTEM_INFO_SYSTEM_INFO_H_
