// The reflash endpoints.
//
//   POST /api/update/bundle      stream a .raucb to /data, never into memory
//   POST /api/update/install     hand the staged bundle to RAUC
//   GET  /api/update/status      slots, operation, and how many tries are left
//   GET  /api/update/events      progress, as server-sent events
//   POST /api/update/mark-good   confirm the running slot
//
// WHAT THIS DELIBERATELY DOES NOT DO: mark a slot good on the console's own
// authority after an install. redline-mark-good.service does that on the next
// boot, gated on redline-dashboard.service, so "good" keeps meaning THE CLUSTER
// PAINTED A FRAME. A console that marked itself good would quietly weaken the
// rollback guarantee to "the web server started", which is not the same promise.

#include "update_routes.h"

#include "core/core.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <system_error>

namespace web_console
{
namespace
{

namespace fs = std::filesystem;
using nlohmann::json;

// A bundle is ~1 GB and the slot it writes into needs room beside it. Refusing
// before a byte is written beats filling /data and failing at 99%.
constexpr std::uint64_t kFreeSpaceMargin = 64ull * 1024ull * 1024ull;

// How many boot attempts the bootloader has left for each entry.
//
// redline-bootloader encodes this in the filename -- boot-b+3-0.conf means three
// tries remaining, none used -- so this is a directory listing rather than a
// call into anything. /boot is mounted read-only and this only reads.
json bootTries()
{
    json entries = json::array();
    std::error_code ec;
    const fs::path dir{"/boot/loader/entries"};
    if (!fs::is_directory(dir, ec))
    {
        return entries;
    }

    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (auto item = bootEntryJson(entry.path().filename().string()))
        {
            entries.push_back(std::move(*item));
        }
    }
    return entries;
}

json slotsJson(rauc_client::Installer& installer, std::string& error)
{
    json slots = json::array();
    for (const auto& slot : installer.slots(&error))
    {
        json item{
            {"name", slot.name},
            {"device", slot.device},
            {"bootname", slot.bootname},
            {"state", slot.state},
            {"boot_status", slot.bootStatus},
            {"status", slot.status},
            {"bundle_version", slot.bundleVersion},
        };
        item["installed_timestamp"] =
            slot.installedTimestamp ? json(*slot.installedTimestamp) : json(nullptr);
        slots.push_back(item);
    }
    return slots;
}

}  // namespace

// A full /data is 507 with the reason, not the 400 "upload aborted" it used to
// be, which sent people looking for a network problem.
Reply storageFailure(int error, const std::string& what, const std::filesystem::path& where)
{
    if (error == ENOSPC || error == EDQUOT)
    {
        return errorReply(507, "ran out of room on " + where.string() + " while " + what);
    }
    return errorReply(500, what + " failed: " + std::strerror(error));
}

std::optional<json> bootEntryJson(const std::string& filename)
{
    // boot-a.conf, boot-b+3.conf, boot-b+2-1.conf
    static const std::regex pattern(R"(^boot-([ab])(?:\+(\d+))?(?:-(\d+))?\.conf$)");
    std::smatch match;
    if (!std::regex_match(filename, match, pattern))
    {
        return std::nullopt;
    }

    // Bounded, because \d+ matches any number of digits and std::stoi threw on
    // one too many. A count that does not fit is not an entry the bootloader
    // wrote, so it is skipped rather than guessed at.
    const auto count = [&match](std::size_t group, json& out) {
        if (!match[group].matched)
        {
            out = nullptr;
            return true;
        }
        const std::string digits = match[group].str();
        int value = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc{} || end != digits.data() + digits.size()) { return false; }
        out = value;
        return true;
    };

    json item;
    item["entry"] = filename;
    item["slot"] = match[1].str();
    // No suffix at all means the entry is not being counted -- it has been
    // marked good, or never needed counting.
    json left;
    json done;
    if (!count(2, left) || !count(3, done))
    {
        return std::nullopt;
    }
    item["tries_left"] = left;
    item["tries_done"] = done;
    return item;
}

BundleSink::BundleSink(int fd, fs::path temporary, fs::path destination, std::uint64_t expected,
                       UploadLease lease)
    : fd_(fd), temporary_(std::move(temporary)), destination_(std::move(destination)),
      expected_(expected), lease_(std::move(lease))
{
}

BundleSink::~BundleSink()
{
    if (fd_ >= 0) { ::close(fd_); }
    // Whatever happened, do not leave a partial file behind.
    if (!renamed_)
    {
        std::error_code ec;
        fs::remove(temporary_, ec);
    }
}

bool BundleSink::write(const char* data, std::size_t length)
{
    if (fd_ < 0 || writeError_ != 0) { return false; }
    while (length > 0)
    {
        const ssize_t written = ::write(fd_, data, length);
        if (written < 0)
        {
            if (errno == EINTR) { continue; }
            writeError_ = errno;
            SPDLOG_ERROR("[update] write to {} failed: {}", temporary_.string(),
                         std::strerror(writeError_));
            return false;
        }
        const auto count = static_cast<std::size_t>(written);  // >= 0, checked above
        data += count;
        length -= count;
        received_ += count;
    }
    return true;
}

Reply BundleSink::finish(bool complete)
{
    if (writeError_ != 0)
    {
        return storageFailure(writeError_, "writing the bundle", destination_.parent_path());
    }
    if (!complete || fd_ < 0)
    {
        SPDLOG_WARN("[update] upload aborted after {} bytes", received_);
        return errorReply(400, "upload aborted after " + std::to_string(received_) + " bytes");
    }

    // A SHORT BODY IS NOT A FINISHED ONE, and until this check existed the
    // difference was invisible here: httplib stops reading at its payload
    // limit, the reader then sees what looks like a clean end of body, and
    // this function happily renamed a truncated file into place. The client
    // got 413 -- httplib replaces the handler's status -- while the board
    // was left holding a corrupt bundle that /api/update/status reported as
    // staged and /api/update/install accepted. RAUC caught it, with
    // "Signature size exceeds bundle size", which is the last line of
    // defence and not where this should be caught.
    //
    // Also covers the ordinary case of a client that disconnects mid-upload.
    if (expected_ > 0 && received_ != expected_)
    {
        SPDLOG_WARN("[update] upload short: {} of {} bytes; not staging",
                    received_, expected_);
        return errorReply(400, "upload incomplete: received " + std::to_string(received_) +
                                   " of " + std::to_string(expected_) + " bytes");
    }

    // fsync before the rename: a rename that lands before the data does is
    // exactly how a power cut produces an installable-looking bundle that is
    // not one. Both it and close() can report a deferred write error, so
    // either failing means the bundle is not known to be on disk.
    const fs::path directory = destination_.parent_path();
    if (::fsync(fd_) != 0)
    {
        const int error = errno;
        SPDLOG_ERROR("[update] fsync {} failed: {}", temporary_.string(), std::strerror(error));
        return storageFailure(error, "flushing the bundle", directory);
    }
    const int closed = ::close(fd_);
    fd_ = -1;
    if (closed != 0)
    {
        const int error = errno;
        SPDLOG_ERROR("[update] close {} failed: {}", temporary_.string(), std::strerror(error));
        return storageFailure(error, "closing the bundle", directory);
    }

    std::error_code ec;
    fs::rename(temporary_, destination_, ec);
    if (ec)
    {
        SPDLOG_ERROR("[update] rename to {} failed: {}", destination_.string(), ec.message());
        return errorReply(500, "could not stage the bundle: " + ec.message());
    }
    renamed_ = true;

    // And the directory, or the rename itself can be lost to a power cut,
    // leaving the previous bundle -- or none -- where this one was reported.
    const int dirFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0 || ::fsync(dirFd) != 0)
    {
        const int error = errno;
        if (dirFd >= 0) { ::close(dirFd); }
        SPDLOG_ERROR("[update] fsync {} failed: {}", directory.string(), std::strerror(error));
        return errorReply(500, std::string("the bundle was staged but may not survive a power cut: ") +
                                   std::strerror(error));
    }
    ::close(dirFd);

    SPDLOG_INFO("[update] staged {} ({} bytes)", destination_.string(), received_);
    return jsonReply(200, json{{"staged", destination_.string()}, {"bytes", received_}});
}

void registerUpdateRoutes(RouteRegistrar& routes, UpdateRoutes& state)
{
    routes.getReply("/api/update/status", [&state] {
        json out;
        out["boot_entries"] = bootTries();

        rauc_client::Installer* installer = state.installer();
        out["rauc_available"] = installer != nullptr;
        if (installer == nullptr)
        {
            const std::string why = "no D-Bus connection for RAUC: " + state.connectError();
            state.reportRauc(false, why);
            out["error"] = why;
            return jsonReply(200, out);
        }

        // GetSlotStatus is the call that activates RAUC if it is not running,
        // and the one whose failure says it cannot be reached -- so it, not
        // the name's owner, decides the "rauc" health check.
        std::string slotsError;
        out["slots"] = slotsJson(*installer, slotsError);
        if (!slotsError.empty())
        {
            state.reportRauc(false, "RAUC did not answer: " + slotsError);
            out["error"] = "RAUC did not answer: " + slotsError;
        }
        else
        {
            state.reportRauc(true, "");
        }

        if (const auto status = installer->status())
        {
            out["operation"] = status->operation;
            out["last_error"] = status->lastError;
            out["compatible"] = status->compatible;
            out["variant"] = status->variant;
            out["boot_slot"] = status->bootSlot;
            out["primary"] = status->primary;
        }

        // file_size fails for a missing file, which is the usual "nothing
        // staged"; checking exists() first and then ignoring this error
        // reported (uintmax_t)-1 bytes for a file that vanished in between.
        std::error_code ec;
        const auto staged = state.stagedBundle();
        const std::uintmax_t stagedBytes = fs::file_size(staged, ec);
        if (!ec)
        {
            out["staged"] = {{"path", staged.string()},
                             {"bytes", static_cast<std::uint64_t>(stagedBytes)}};
        }
        else
        {
            out["staged"] = nullptr;
        }
        return jsonReply(200, out);
    });

    routes.postUpload(
        "/api/update/bundle",
        [&state](std::uint64_t contentLength, Reply& refusal) -> std::unique_ptr<UploadSink> {
            UploadLease lease = state.tryBeginUpload();
            if (!lease)
            {
                SPDLOG_WARN("[update] upload refused: another is in progress");
                refusal = errorReply(409, "another upload is in progress");
                return nullptr;
            }

            // Refuse while an install is running: overwriting the bundle RAUC is
            // reading is a way to corrupt a slot, not a race worth allowing.
            // 409, not 507 -- this is a conflict, not a storage problem, and the
            // operator needs to be able to tell those apart. The cached
            // property only: an upload should not wait on a D-Bus round trip,
            // and a RAUC that has never been activated is not installing.
            if (rauc_client::Installer* installer = state.installer())
            {
                if (const std::string operation = installer->operation();
                    !operation.empty() && operation != "idle")
                {
                    SPDLOG_WARN("[update] upload refused: RAUC is {}", operation);
                    refusal = errorReply(409, "RAUC is " + operation +
                                                  "; wait for it to finish before uploading");
                    return nullptr;
                }
            }

            std::error_code ec;
            fs::create_directories(state.uploadDir(), ec);

            // The staged bundle is about to be replaced, so its space counts as
            // free -- but only once it is gone, since the new one is written
            // beside it. It is removed up front only when that is what makes
            // the upload fit.
            const auto staged = state.stagedBundle();
            std::error_code sizeError;
            const std::uintmax_t stagedSize = fs::file_size(staged, sizeError);
            const std::uint64_t stagedBytes = sizeError ? 0 : static_cast<std::uint64_t>(stagedSize);

            const auto space = fs::space(state.uploadDir(), ec);
            const std::uint64_t needed = contentLength + kFreeSpaceMargin;
            if (!ec && contentLength > 0 && space.available < needed)
            {
                if (space.available + stagedBytes >= needed)
                {
                    SPDLOG_INFO("[update] removing the staged bundle ({} bytes) to make room",
                                stagedBytes);
                    fs::remove(staged, ec);
                }
                else
                {
                    SPDLOG_WARN("[update] upload refused: {} bytes free, {} staged, {} needed",
                                space.available, stagedBytes, needed);
                    // The numbers, because "no space" without them tells whoever
                    // is at the bench nothing about how much to free.
                    std::string message = "not enough room on " + state.uploadDir().string() +
                                          ": " + std::to_string(space.available) +
                                          " bytes free, " + std::to_string(needed) +
                                          " needed (the bundle plus " +
                                          std::to_string(kFreeSpaceMargin) + " bytes of headroom)";
                    if (stagedBytes > 0)
                    {
                        message += ", counting the " + std::to_string(stagedBytes) +
                                   " bytes of the staged bundle this would replace";
                    }
                    refusal = errorReply(507, message);
                    return nullptr;
                }
            }

            // mkstemps rather than a fixed name: O_EXCL, and a name no other
            // upload or leftover can share.
            std::string temporary = (state.uploadDir() / ".incoming-XXXXXX.raucb").string();
            const int fd = ::mkstemps(temporary.data(), static_cast<int>(std::strlen(".raucb")));
            if (fd < 0)
            {
                const int error = errno;
                SPDLOG_ERROR("[update] cannot create a file in {}: {}", state.uploadDir().string(),
                             std::strerror(error));
                refusal = storageFailure(error, "creating the upload file", state.uploadDir());
                return nullptr;
            }
            return std::make_unique<BundleSink>(fd, temporary, staged, contentLength,
                                                std::move(lease));
        });

    routes.post("/api/update/install", [&state](const std::string&) {
        rauc_client::Installer* installer = state.installer();
        if (installer == nullptr)
        {
            return errorReply(503, "no D-Bus connection for RAUC: " + state.connectError());
        }

        std::error_code ec;
        const auto bundle = state.stagedBundle();
        if (!fs::exists(bundle, ec))
        {
            return errorReply(404, "no bundle has been uploaded");
        }

        std::string error;
        if (!installer->installBundle(bundle.string(), error))
        {
            // RAUC busy is the common one, and 409 is the honest status for it.
            const bool busy = error.find("busy") != std::string::npos;
            SPDLOG_WARN("[update] install refused: {}", error);
            return errorReply(busy ? 409 : 500, error);
        }

        SPDLOG_INFO("[update] installing {}", bundle.string());
        return jsonReply(202, json{{"installing", bundle.string()}});
    });

    routes.post("/api/update/mark-good", [&state](const std::string&) {
        rauc_client::Installer* installer = state.installer();
        if (installer == nullptr)
        {
            return errorReply(503, "no D-Bus connection for RAUC: " + state.connectError());
        }

        std::string slotName;
        std::string message;
        std::string error;
        if (!installer->mark("good", "booted", slotName, message, error))
        {
            return errorReply(500, error);
        }
        SPDLOG_INFO("[update] marked {} good: {}", slotName, message);
        return jsonReply(200, json{{"slot", slotName}, {"message", message}});
    });

    routes.events("/api/update/events", state.events());
}

}  // namespace web_console
