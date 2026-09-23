// What the console's routes make of a client's input, checked without a socket:
// a wrong-typed field is a 400 with the field named, never an exception, and a
// timeout is clamped rather than trusted.

#include "service_routes.h"
#include "update_routes.h"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{

using namespace web_console;
using nlohmann::json;
using std::chrono::milliseconds;

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

json validCall()
{
    return json{{"key", "nodes/fake_backlight/set_brightness"},
                {"request_schema", "SetBrightnessRequest"},
                {"response_schema", "SetBrightnessResponse"},
                {"fields", {{"level", 3}}}};
}

// Parses without letting anything escape: an exception here is exactly the
// bug, so it is caught and reported as a failure rather than aborting the run.
bool parses(const json& body, pub_sub::ServiceCallRequest& call, std::string& error)
{
    try
    {
        return parseCallRequest(body, call, error);
    }
    catch (const std::exception& e)
    {
        check(false, std::string("parseCallRequest threw: ") + e.what() + " on " + body.dump());
        return false;
    }
}

void testValidCall()
{
    pub_sub::ServiceCallRequest call;
    std::string error;
    check(parses(validCall(), call, error), "a well-formed call parses");
    check(call.key == "nodes/fake_backlight/set_brightness", "key is read");
    check(call.fields == json{{"level", 3}}, "fields are read");
    check(call.timeout == milliseconds(2000), "no timeout_ms keeps the default");
}

void testWrongTypesAreRefusedNotThrown()
{
    const auto refused = [](json body, const std::string& field, const std::string& what) {
        pub_sub::ServiceCallRequest call;
        std::string error;
        check(!parses(body, call, error), what + " is refused");
        check(error.find(field) != std::string::npos,
              what + " names the field (got '" + error + "')");
    };

    json body = validCall();
    body["key"] = 42;
    refused(body, "key", "a numeric key");

    body = validCall();
    body["request_schema"] = nullptr;
    refused(body, "request_schema", "a null request_schema");

    body = validCall();
    body.erase("response_schema");
    refused(body, "response_schema", "a missing response_schema");

    body = validCall();
    body["key"] = "";
    refused(body, "key", "an empty key");

    body = validCall();
    body["fields"] = json::array({1, 2});
    refused(body, "fields", "fields as an array");

    body = validCall();
    body["fields"] = "level=3";
    refused(body, "fields", "fields as a string");

    body = validCall();
    body["timeout_ms"] = "500";
    refused(body, "timeout_ms", "a string timeout_ms");

    body = validCall();
    body["timeout_ms"] = true;
    refused(body, "timeout_ms", "a boolean timeout_ms");

    refused(json::array({validCall()}), "object", "a body that is an array");
    refused(json("key"), "object", "a body that is a string");
}

void testTimeoutIsClamped()
{
    const auto timeoutFor = [](json value) {
        json body = validCall();
        body["timeout_ms"] = std::move(value);
        pub_sub::ServiceCallRequest call;
        std::string error;
        check(parses(body, call, error), "timeout_ms " + body["timeout_ms"].dump() + " parses");
        return call.timeout;
    };

    check(timeoutFor(500) == milliseconds(500), "a timeout inside the range is kept");
    check(timeoutFor(0) == kMinCallTimeout, "zero is raised to the floor");
    check(timeoutFor(-5) == kMinCallTimeout, "a negative timeout is raised to the floor");
    check(timeoutFor(600000) == kMaxCallTimeout, "ten minutes is cut to the ceiling");
    check(timeoutFor(1e300) == kMaxCallTimeout, "a huge double cannot overflow");
    check(timeoutFor(json(18446744073709551615ULL)) == kMaxCallTimeout,
          "nor can the largest unsigned");
    check(timeoutFor(250.7) == milliseconds(250), "a fractional timeout is truncated");
}

namespace fs = std::filesystem;

struct ScratchDir
{
    ScratchDir()
    {
        std::string pattern = (fs::temp_directory_path() / "web_console_test_XXXXXX").string();
        if (::mkdtemp(pattern.data()) != nullptr) { path = pattern; }
    }
    ~ScratchDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

// A full /data used to come back as 400 "upload aborted", which reads as a
// network problem.
void testStorageFailureStatus()
{
    const Reply full = storageFailure(ENOSPC, "writing the bundle", "/data");
    check(full.status == 507, "ENOSPC is 507 (got " + std::to_string(full.status) + ")");
    check(full.body.find("room") != std::string::npos, "and says it ran out of room");
    check(storageFailure(EDQUOT, "writing the bundle", "/data").status == 507,
          "EDQUOT is 507 too");
    check(storageFailure(EIO, "writing the bundle", "/data").status == 500,
          "any other error is 500");
}

// The write error has to survive to finish(). A read-only fd fails every
// write on any host; testFullDiskIs507 is the same path with the real errno.
void testWriteErrorReachesTheReply()
{
    ScratchDir dir;
    const fs::path temporary = dir.path / ".incoming-ro.raucb";
    const fs::path staged = dir.path / "bundle.raucb";
    ::close(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    const int fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    BundleSink sink(fd, temporary, staged, 4096, UploadLease{});

    const std::string chunk(4096, 'x');
    check(!sink.write(chunk.data(), chunk.size()), "a write to a read-only fd fails");
    const Reply reply = sink.finish(true);
    check(reply.status == 500, "a failed write is 500, not 400 (got " + std::to_string(reply.status) + ")");
    check(!fs::exists(staged), "nothing is staged after a failed write");
}

#if defined(__linux__)
// /dev/full answers every write with ENOSPC, which is a full /data without
// having to fill one. Linux-only: macOS has no such device, and the target
// and the Yocto builder are both Linux.
void testFullDiskIs507()
{
    ScratchDir dir;
    const int fd = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        check(false, "/dev/full can be opened");
        return;
    }
    const fs::path temporary = dir.path / ".incoming-test.raucb";
    const fs::path staged = dir.path / "bundle.raucb";
    BundleSink sink(fd, temporary, staged, 4096, UploadLease{});

    const std::string chunk(4096, 'x');
    check(!sink.write(chunk.data(), chunk.size()), "a write to a full disk fails");
    const Reply reply = sink.finish(false);
    check(reply.status == 507, "a full disk is 507 (got " + std::to_string(reply.status) + ")");
    check(reply.body.find("room") != std::string::npos, "and says it ran out of room");
    check(!fs::exists(staged), "nothing is staged");
}
#endif

void testAbortedAndShortUploadsStageNothing()
{
    ScratchDir dir;
    const fs::path staged = dir.path / "bundle.raucb";
    const std::string chunk(100, 'x');

    {
        const fs::path temporary = dir.path / ".incoming-a.raucb";
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        BundleSink sink(fd, temporary, staged, 200, UploadLease{});
        check(sink.write(chunk.data(), chunk.size()), "a write to a real file succeeds");
        check(sink.finish(false).status == 400, "a client that went away is 400");
    }
    check(!fs::exists(staged), "an aborted upload stages nothing");
    check(!fs::exists(dir.path / ".incoming-a.raucb"), "and leaves no temporary");

    {
        const fs::path temporary = dir.path / ".incoming-b.raucb";
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        BundleSink sink(fd, temporary, staged, 200, UploadLease{});
        check(sink.write(chunk.data(), chunk.size()), "a write to a real file succeeds");
        check(sink.finish(true).status == 400, "a body shorter than Content-Length is 400");
    }
    check(!fs::exists(staged), "a short upload stages nothing");

    {
        const fs::path temporary = dir.path / ".incoming-c.raucb";
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        BundleSink sink(fd, temporary, staged, 100, UploadLease{});
        check(sink.write(chunk.data(), chunk.size()), "a write to a real file succeeds");
        check(sink.finish(true).status == 200, "a complete upload is staged");
    }
    std::error_code ec;
    check(fs::file_size(staged, ec) == 100 && !ec, "with every byte");
}

void testBootEntries()
{
    const auto entry = bootEntryJson("boot-b+3-0.conf");
    check(entry.has_value(), "boot-b+3-0.conf is an entry");
    if (entry)
    {
        check((*entry)["slot"] == "b", "its slot is read");
        check((*entry)["tries_left"] == 3, "its tries left are read");
        check((*entry)["tries_done"] == 0, "its tries done are read");
    }

    const auto counted = bootEntryJson("boot-a.conf");
    check(counted && (*counted)["tries_left"].is_null(), "no suffix means not being counted");

    check(!bootEntryJson("boot-c.conf"), "an unknown slot is not an entry");
    check(!bootEntryJson("boot-a+.conf"), "an empty count is not an entry");
    check(!bootEntryJson("loader.conf"), "an unrelated file is not an entry");

    // std::stoi threw std::out_of_range on this, out of the status handler.
    bool threw = false;
    try
    {
        check(!bootEntryJson("boot-b+99999999999999999999-0.conf"),
              "a count too large for an int is skipped");
        check(!bootEntryJson("boot-b+1-99999999999999999999.conf"),
              "and so is a done count too large");
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    check(!threw, "an oversized count does not throw");
}

}  // namespace

int main()
{
    testValidCall();
    testWrongTypesAreRefusedNotThrown();
    testTimeoutIsClamped();
    testStorageFailureStatus();
    testWriteErrorReachesTheReply();
#if defined(__linux__)
    testFullDiskIs507();
#endif
    testAbortedAndShortUploadsStageNothing();
    testBootEntries();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
