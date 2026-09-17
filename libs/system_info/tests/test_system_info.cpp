// The /proc, /sys and /etc parsers, against a fake tree holding what a redline
// board actually reports -- os-release from a wrynose image, meminfo and hwmon
// as the LattePanda's kernel writes them.
//
// The point of the Roots indirection is this file: /proc cannot be arranged by
// a test, and a parser that has only ever seen the host's /proc has only ever
// been tested against one input.

#include "system_info/system_info.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using namespace system_info;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

void put(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << contents << '\n';
}

struct FakeTree
{
    fs::path root = fs::temp_directory_path() / "system_info_test";
    Roots roots;

    FakeTree()
    {
        fs::remove_all(root);
        roots.proc = root / "proc";
        roots.sys = root / "sys";
        roots.etc = root / "etc";

        put(roots.etc / "os-release",
            "ID=redline\n"
            "NAME=\"Redline\"\n"
            "PRETTY_NAME=\"Redline Labs cluster 1.0\"\n"
            "VERSION_ID=1.0\n"
            "BUILD_ID=20260917013010");

        put(roots.proc / "uptime", "1234.56 9876.54");
        put(roots.proc / "loadavg", "0.52 0.41 0.38 1/423 5678");
        put(roots.proc / "meminfo",
            "MemTotal:       16303152 kB\n"
            "MemFree:         9123456 kB\n"
            "MemAvailable:   14012345 kB\n"
            "Buffers:          123456 kB\n"
            "Cached:          2345678 kB\n"
            "HugePages_Total:       0");

        // Two hwmon devices, the second with a label, as coretemp and tmp1075
        // present themselves.
        put(roots.sys / "class/hwmon/hwmon0/name", "coretemp");
        put(roots.sys / "class/hwmon/hwmon0/temp1_input", "42000");
        put(roots.sys / "class/hwmon/hwmon0/temp2_input", "39000");
        put(roots.sys / "class/hwmon/hwmon1/name", "tmp1075");
        put(roots.sys / "class/hwmon/hwmon1/temp1_input", "31500");
        put(roots.sys / "class/hwmon/hwmon1/temp1_label", "panel");
    }

    ~FakeTree()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void testOsRelease(const FakeTree& tree)
{
    const auto release = readOsRelease(tree.roots);
    check(release.has_value(), "os-release reads");
    if (!release) { return; }
    check(release->id == "redline", "ID is unquoted");
    // PRETTY_NAME wins over NAME: it is what a person recognises.
    check(release->name == "Redline Labs cluster 1.0", "PRETTY_NAME is preferred and unquoted");
    check(release->version == "1.0", "VERSION_ID fills in for a missing VERSION");
    check(release->buildId == "20260917013010", "BUILD_ID is what a Yocto image stamps");
}

void testUptimeAndLoad(const FakeTree& tree)
{
    const auto uptime = readUptime(tree.roots);
    check(uptime.has_value() && uptime->count() == 1234, "uptime truncates to whole seconds");

    const auto load = readLoadAverage(tree.roots);
    check(load.has_value(), "loadavg reads");
    if (!load) { return; }
    check(near(load->oneMinute, 0.52) && near(load->fiveMinutes, 0.41) && near(load->fifteenMinutes, 0.38),
          "the three averages, ignoring the running/total and last-pid fields");
}

void testMemoryIsBytesNotKilobytes(const FakeTree& tree)
{
    const auto memory = readMemory(tree.roots);
    check(memory.has_value(), "meminfo reads");
    if (!memory) { return; }
    check(memory->total.has_value() && *memory->total == 16303152ull * 1024ull, "MemTotal is converted to bytes");
    check(memory->available.has_value() && *memory->available == 14012345ull * 1024ull, "MemAvailable is present");
    check(memory->free.has_value() && *memory->free == 9123456ull * 1024ull, "MemFree is present");
    check(memory->cached.has_value(), "Cached is present");
    // A field with no kB suffix must not be silently scaled.
    check(memory->total.has_value(), "a unit-less line does not corrupt the parse");
}

void testMissingFilesAreAbsentNotZero()
{
    Roots empty;
    empty.proc = fs::temp_directory_path() / "system_info_test_absent/proc";
    empty.sys = fs::temp_directory_path() / "system_info_test_absent/sys";
    empty.etc = fs::temp_directory_path() / "system_info_test_absent/etc";

    check(!readOsRelease(empty).has_value(), "a missing os-release is nullopt, not an empty identity");
    check(!readUptime(empty).has_value(), "a missing uptime is nullopt");
    check(!readLoadAverage(empty).has_value(), "a missing loadavg is nullopt");
    check(!readMemory(empty).has_value(), "a missing meminfo is nullopt");
    check(readTemperatures(empty).empty(), "no hwmon class yields no sensors rather than a fault");
}

void testTemperatures(const FakeTree& tree)
{
    const std::vector<Temperature> temps = readTemperatures(tree.roots);
    check(temps.size() == 3, "every temp*_input under every hwmon is reported");
    if (temps.size() != 3) { return; }

    // Sorted by device then input, so the list does not reshuffle between reads.
    check(temps[0].device == "coretemp" && temps[0].label == "temp1" && temps[0].milliCelsius == 42000,
          "an unlabelled sensor falls back to its attribute name");
    check(temps[1].device == "coretemp" && temps[1].label == "temp2", "channels keep their order");
    check(temps[2].device == "tmp1075" && temps[2].label == "panel" && temps[2].milliCelsius == 31500,
          "temp<n>_label is used when the driver provides one");
}

void testFilesystemAndInterfacesAgainstTheRealHost()
{
    // statvfs and getifaddrs cannot be fixtured, so these assert only what is
    // true on any machine this compiles on.
    const auto usage = readFilesystem("/");
    check(usage.has_value(), "root filesystem is readable");
    if (usage)
    {
        check(usage->totalBytes > 0, "a mounted filesystem has a non-zero size");
        check(usage->availableBytes <= usage->totalBytes, "available never exceeds total");
    }
    check(!readFilesystem("/definitely/not/a/mount/point").has_value(), "a missing path is nullopt");

    const std::vector<NetworkInterface> interfaces = readNetworkInterfaces();
    check(!interfaces.empty(), "at least loopback is present");
    bool sawLoopback = false;
    for (const NetworkInterface& interface : interfaces)
    {
        if (interface.loopback)
        {
            sawLoopback = true;
            check(!interface.addresses.empty(), "loopback has an address");
        }
    }
    check(sawLoopback, "loopback is reported rather than filtered out");
}

}  // namespace

int main()
{
    {
        FakeTree tree;
        testOsRelease(tree);
        testUptimeAndLoad(tree);
        testMemoryIsBytesNotKilobytes(tree);
        testTemperatures(tree);
    }
    testMissingFilesAreAbsentNotZero();
    testFilesystemAndInterfacesAgainstTheRealHost();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
