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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>

namespace web_console
{
namespace
{

namespace fs = std::filesystem;
using nlohmann::json;

// A bundle is ~338 MB and the slot it writes into needs room beside it. Refusing
// before a byte is written beats filling /data and failing at 99%.
constexpr std::uint64_t kFreeSpaceMargin = 64ull * 1024ull * 1024ull;

Reply jsonReply(int status, const json& body)
{
    return Reply{.status = status, .body = body.dump(), .contentType = "application/json"};
}

Reply errorReply(int status, const std::string& message)
{
    return jsonReply(status, json{{"error", message}});
}

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

    // boot-a.conf, boot-b+3.conf, boot-b+2-1.conf
    static const std::regex pattern(R"(^boot-([ab])(?:\+(\d+))?(?:-(\d+))?\.conf$)");
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        std::smatch match;
        const std::string name = entry.path().filename().string();
        if (!std::regex_match(name, match, pattern))
        {
            continue;
        }
        json item;
        item["entry"] = name;
        item["slot"] = match[1].str();
        // No suffix at all means the entry is not being counted -- it has been
        // marked good, or never needed counting.
        item["tries_left"] = match[2].matched ? json(std::stoi(match[2].str())) : json(nullptr);
        item["tries_done"] = match[3].matched ? json(std::stoi(match[3].str())) : json(nullptr);
        entries.push_back(item);
    }
    return entries;
}

json slotsJson(rauc_client::Installer& installer)
{
    json slots = json::array();
    for (const auto& slot : installer.slots())
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

// Streams an upload straight to disk.
//
// Writes to a dot-prefixed temporary and renames only on success, so RAUC can
// never be handed a half-written bundle: the rename is atomic within the
// filesystem, and a crashed or abandoned upload leaves a .incoming-* file that
// the node sweeps at startup rather than something that looks installable.
class BundleSink : public UploadSink
{
public:
    BundleSink(fs::path temporary, fs::path destination)
        : temporary_(std::move(temporary)), destination_(std::move(destination)),
          out_(temporary_, std::ios::binary | std::ios::trunc)
    {
        if (!out_)
        {
            SPDLOG_ERROR("[update] cannot open {}: {}", temporary_.string(), std::strerror(errno));
        }
    }

    ~BundleSink() override
    {
        // Whatever happened, do not leave a partial file behind.
        if (!renamed_)
        {
            out_.close();
            std::error_code ec;
            fs::remove(temporary_, ec);
        }
    }

    bool write(const char* data, std::size_t length) override
    {
        if (!out_) { return false; }
        out_.write(data, static_cast<std::streamsize>(length));
        if (!out_)
        {
            SPDLOG_ERROR("[update] write to {} failed: {}", temporary_.string(), std::strerror(errno));
            return false;
        }
        received_ += length;
        return true;
    }

    Reply finish(bool complete) override
    {
        if (!complete || !out_)
        {
            SPDLOG_WARN("[update] upload aborted after {} bytes", received_);
            return errorReply(400, "upload aborted");
        }

        out_.flush();
        out_.close();

        // fsync before the rename: a rename that lands before the data does is
        // exactly how a power cut produces an installable-looking bundle that
        // is not one.
        {
            const int fd = ::open(temporary_.c_str(), O_RDONLY);
            if (fd >= 0)
            {
                ::fsync(fd);
                ::close(fd);
            }
        }

        std::error_code ec;
        fs::rename(temporary_, destination_, ec);
        if (ec)
        {
            SPDLOG_ERROR("[update] rename to {} failed: {}", destination_.string(), ec.message());
            return errorReply(500, "could not stage the bundle: " + ec.message());
        }
        renamed_ = true;

        SPDLOG_INFO("[update] staged {} ({} bytes)", destination_.string(), received_);
        return jsonReply(200, json{{"staged", destination_.string()}, {"bytes", received_}});
    }

private:
    fs::path temporary_;
    fs::path destination_;
    std::ofstream out_;
    std::uint64_t received_ { 0 };
    bool renamed_ { false };
};

}  // namespace

void registerUpdateRoutes(RouteRegistrar& routes, UpdateRoutes& state)
{
    routes.getReply("/api/update/status", [&state] {
        json out;
        out["rauc_available"] = state.available();
        out["boot_entries"] = bootTries();

        rauc_client::Installer* installer = state.installer();
        if (installer == nullptr || !state.available())
        {
            out["error"] = "RAUC is not reachable on D-Bus";
            return jsonReply(200, out);
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
        out["slots"] = slotsJson(*installer);

        // What is staged and waiting, so the page can offer Install without a
        // fresh upload.
        std::error_code ec;
        const auto staged = state.stagedBundle();
        if (fs::exists(staged, ec))
        {
            out["staged"] = {{"path", staged.string()},
                             {"bytes", static_cast<std::uint64_t>(fs::file_size(staged, ec))}};
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
            rauc_client::Installer* installer = state.installer();

            // Refuse while an install is running: overwriting the bundle RAUC is
            // reading is a way to corrupt a slot, not a race worth allowing.
            // 409, not 507 -- this is a conflict, not a storage problem, and the
            // operator needs to be able to tell those apart.
            if (installer != nullptr && state.available())
            {
                if (const auto status = installer->status();
                    status && !status->operation.empty() && status->operation != "idle")
                {
                    SPDLOG_WARN("[update] upload refused: RAUC is {}", status->operation);
                    refusal = errorReply(409, "RAUC is " + status->operation +
                                                  "; wait for it to finish before uploading");
                    return nullptr;
                }
            }

            std::error_code ec;
            fs::create_directories(state.uploadDir(), ec);

            const auto space = fs::space(state.uploadDir(), ec);
            if (!ec && contentLength > 0 && space.available < contentLength + kFreeSpaceMargin)
            {
                const std::uint64_t needed = contentLength + kFreeSpaceMargin;
                SPDLOG_WARN("[update] upload refused: {} bytes free, {} needed",
                            space.available, needed);
                // The numbers, because "no space" without them tells whoever is
                // at the bench nothing about how much to free.
                refusal = errorReply(507, "not enough room on " + state.uploadDir().string() +
                                              ": " + std::to_string(space.available) +
                                              " bytes free, " + std::to_string(needed) +
                                              " needed");
                return nullptr;
            }

            const auto temporary = state.uploadDir() /
                                   (".incoming-" + std::to_string(::getpid()) + ".raucb");
            return std::make_unique<BundleSink>(temporary, state.stagedBundle());
        });

    routes.post("/api/update/install", [&state](const std::string&) {
        rauc_client::Installer* installer = state.installer();
        if (installer == nullptr || !state.available())
        {
            return errorReply(503, "RAUC is not reachable on D-Bus");
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
        if (installer == nullptr || !state.available())
        {
            return errorReply(503, "RAUC is not reachable on D-Bus");
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
