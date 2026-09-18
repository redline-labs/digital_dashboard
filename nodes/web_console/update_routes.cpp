#include "update_routes.h"

#include "core/core.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <system_error>

namespace web_console
{
namespace fs = std::filesystem;

UploadLease::~UploadLease()
{
    if (busy_ != nullptr) { busy_->store(false); }
}

UpdateRoutes::UpdateRoutes(const NodeConfig& config) : config_(config) {}

UploadLease UpdateRoutes::tryBeginUpload()
{
    bool expected = false;
    if (!uploading_.compare_exchange_strong(expected, true)) { return {}; }
    return UploadLease(uploading_);
}

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

    const std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    return connectLocked(error);
}

bool UpdateRoutes::connectLocked(std::string& error)
{
    lastAttempt_ = std::chrono::steady_clock::now();

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

    auto installer = std::make_unique<rauc_client::Installer>(bus, callbacks);
    if (!installer->connect(error))
    {
        connectError_ = error;
        return false;
    }
    installer_ = std::move(installer);
    connectError_.clear();
    return true;
}

rauc_client::Installer* UpdateRoutes::installer()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    // Rate limited: a bus that is down stays down for a while, and every
    // status poll would otherwise pay for a failed connect.
    constexpr auto kRetryEvery = std::chrono::seconds(5);
    if (installer_ == nullptr && started_ &&
        std::chrono::steady_clock::now() - lastAttempt_ >= kRetryEvery)
    {
        std::string error;
        if (connectLocked(error))
        {
            SPDLOG_INFO("[update] reached RAUC on the {} bus", config_.raucBus);
        }
    }
    return installer_.get();
}

bool UpdateRoutes::available()
{
    return installer() != nullptr;
}

std::string UpdateRoutes::connectError()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return connectError_.empty() ? std::string("not connected") : connectError_;
}

void UpdateRoutes::onRaucHealth(std::function<void(bool, const std::string&)> callback)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    raucHealth_ = std::move(callback);
    lastReport_.reset();
}

void UpdateRoutes::reportRauc(bool ok, const std::string& detail)
{
    std::function<void(bool, const std::string&)> callback;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (lastReport_ && lastReport_->first == ok && lastReport_->second == detail) { return; }
        lastReport_ = std::pair{ok, detail};
        callback = raucHealth_;
    }
    if (callback) { callback(ok, detail); }
}

}  // namespace web_console
