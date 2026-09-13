// libs/core: paths and the systemd notification, exercised without a target.
#include "core/core.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition)
    {
        ++failures;
    }
}

void testDataDir()
{
    ::setenv("REDLINE_DATA_DIR", "/data", 1);
    check(core::paths::dataDir() == "/data", "REDLINE_DATA_DIR wins when set");

    ::unsetenv("REDLINE_DATA_DIR");
    ::setenv("HOME", "/home/someone", 1);
    ::unsetenv("XDG_DATA_HOME");
    const std::string fallback = core::paths::dataDir();
    check(fallback.rfind("/home/someone/", 0) == 0 && fallback.find("redline") != std::string::npos,
          "unset falls back to a per-user directory under HOME: " + fallback);
}

void testExpand()
{
    ::setenv("REDLINE_DATA_DIR", "/data", 1);
    check(core::paths::expand("${REDLINE_DATA_DIR}/maps/socal.mbtiles") == "/data/maps/socal.mbtiles",
          "${REDLINE_DATA_DIR} expands to the data directory");

    ::unsetenv("REDLINE_DATA_DIR");
    ::setenv("HOME", "/home/someone", 1);
    ::unsetenv("XDG_DATA_HOME");
    const std::string unset = core::paths::expand("${REDLINE_DATA_DIR}/maps/socal.mbtiles");
    check(unset.rfind("/home/someone/", 0) == 0 && unset.find("/maps/socal.mbtiles") != std::string::npos,
          "${REDLINE_DATA_DIR} still expands when the variable is unset: " + unset);

    ::setenv("SOME_VAR", "xyz", 1);
    check(core::paths::expand("a/${SOME_VAR}/b") == "a/xyz/b", "other ${VARS} expand from the environment");
    check(core::paths::expand("a/${NOT_SET_ANYWHERE}/b") == "a//b", "an unset variable expands to nothing");
    check(core::paths::expand("~/x") == "/home/someone/x", "~/ is the home directory");
    check(core::paths::expand("rel/file", "/etc/redline") == "/etc/redline/rel/file",
          "a relative path resolves against the config directory");
    check(core::paths::expand("/abs/file", "/etc/redline") == "/abs/file", "an absolute path is left alone");
    check(core::paths::expand("rel/file") == "rel/file", "a relative path with no base is left alone");
}

void testExecutableAndResource()
{
    const std::string dir = core::paths::executableDir();
    check(!dir.empty() && std::filesystem::exists(dir), "the executable directory is found: " + dir);

    // A file that exists in the checkout resolves to it; nothing next to a
    // build-tree binary is called eds/.
    const std::string eds = core::paths::resource("eds/grayhill/DS401_3K_C.eds");
    check(std::filesystem::exists(eds), "a shipped resource resolves to an existing file: " + eds);
    check(core::paths::resource("no/such/file") == "no/such/file", "an unknown resource is returned unchanged");
}

void testNotify()
{
    ::unsetenv("NOTIFY_SOCKET");
    check(!core::systemd::notifyReady(), "no NOTIFY_SOCKET: nothing sent, false returned");

    // A real datagram socket, the way systemd provides one.
    char tmpl[] = "/tmp/core_test_notify_XXXXXX";
    const int dirfd = ::mkstemp(tmpl);
    check(dirfd >= 0, "temp path");
    ::close(dirfd);
    ::unlink(tmpl);
    const std::string path = tmpl;

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    check(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind a notify socket");

    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    check(core::systemd::notifyReady(), "READY=1 is sent when NOTIFY_SOCKET is set");

    char buf[64] = {};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    check(n > 0 && std::string(buf, static_cast<size_t>(n)) == "READY=1\n", "and it is exactly READY=1");
    ::close(fd);
    ::unlink(path.c_str());
    ::unsetenv("NOTIFY_SOCKET");
}

}  // namespace

int main()
{
    testDataDir();
    testExpand();
    testExecutableAndResource();
    testNotify();
    std::printf("%s\n", failures == 0 ? "all core tests passed" : "core tests FAILED");
    return failures == 0 ? 0 : 1;
}
