#include "update_routes.h"

#include "core/core.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <system_error>

namespace web_console
{
namespace fs = std::filesystem;

UpdateRoutes::UpdateRoutes(const NodeConfig& config) : config_(config) {}

UpdateRoutes::~UpdateRoutes()
{
    // Wake any browser still holding an SSE connection so its thread can end.
    events_.close();
}

fs::path UpdateRoutes::uploadDir() const
{
    return fs::path(config_.uploadDir);
}

fs::path UpdateRoutes::stagedBundle() const
{
    return uploadDir() / "bundle.raucb";
}

bool UpdateRoutes::start(std::string& error)
{
    // A crashed or abandoned upload leaves a .incoming-* file. Sweeping at
    // startup keeps them from accumulating on a partition that has a bundle's
    // worth of headroom and no more.
    std::error_code ec;
    fs::create_directories(uploadDir(), ec);
    for (const auto& entry : fs::directory_iterator(uploadDir(), ec))
    {
        if (entry.path().filename().string().starts_with(".incoming-"))
        {
            SPDLOG_INFO("[update] removing stale upload {}", entry.path().string());
            fs::remove(entry.path(), ec);
        }
    }

    rauc_client::Installer::Callbacks callbacks;

    // Progress and completion arrive on GDBus's thread. Publishing only appends
    // to each subscriber's queue, so a slow browser cannot stall an install.
    callbacks.onProgress = [this](const rauc_client::Progress& progress) {
        events_.publish("progress", nlohmann::json{
                                        {"percentage", progress.percentage},
                                        {"message", progress.message},
                                        {"nesting", progress.nesting},
                                    }
                                        .dump());
    };

    callbacks.onCompleted = [this](std::int32_t result, const std::string& lastError) {
        SPDLOG_INFO("[update] install completed: result={} {}", result, lastError);
        events_.publish("completed", nlohmann::json{
                                         {"result", result},
                                         {"ok", result == 0},
                                         {"last_error", lastError},
                                     }
                                         .dump());
    };

    const auto bus = config_.raucBus == "session" ? rauc_client::Installer::Bus::Session
                                                  : rauc_client::Installer::Bus::System;
    if (config_.raucBus == "session")
    {
        // Only ever for development against tools/rauc_stub.
        SPDLOG_WARN("[update] using the SESSION bus for RAUC -- development only");
    }

    installer_ = std::make_unique<rauc_client::Installer>(bus, callbacks);
    if (!installer_->connect(error))
    {
        installer_.reset();
        return false;
    }
    return true;
}

bool UpdateRoutes::available() const
{
    // serviceAvailable(), not connected(): a proxy exists for a name nobody
    // owns, and on the image rauc.service is bus-activated, so this is the only
    // honest answer to "can we reflash right now".
    return installer_ != nullptr && installer_->serviceAvailable();
}

}  // namespace web_console
