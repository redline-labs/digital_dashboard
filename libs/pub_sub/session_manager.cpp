#include "pub_sub/session_manager.h"
#include "spdlog/spdlog.h"
#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pub_sub
{

std::mutex SessionManager::mutex_ = {};
std::weak_ptr<zenoh::Session> SessionManager::weak_session_ = {};
std::vector<std::pair<std::string, std::string>> SessionManager::config_overrides_ = {};

void SessionManager::insertConfig(std::string key, std::string value)
{
    std::lock_guard<std::mutex> lk(mutex_);
    config_overrides_.emplace_back(std::move(key), std::move(value));
}

zenoh::Config SessionManager::buildConfig()
{
    zenoh::Config config = zenoh::Config::create_default();

    // Prefer local peer-to-peer discovery by default for dev. Applied *before*
    // the caller's overrides, so "callers can override this" is actually true.
    // It used to be inserted afterwards, which silently clobbered any mode a
    // caller had set through insertConfig().
    config.insert_json5("mode", "\"peer\"");

    for (const auto& [key, value] : config_overrides_)
    {
        try
        {
            config.insert_json5(key, value);
        }
        catch (const std::exception& e)
        {
            // One bad setting should not cost us the whole session; name it and
            // carry on. Reported here rather than in insertConfig() because
            // that is where we have a real config to validate against.
            SPDLOG_ERROR("Ignoring zenoh config override '{}' = '{}': {}", key, value, e.what());
        }
    }

    return config;
}

std::shared_ptr<zenoh::Session> SessionManager::getOrCreate()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (auto existing = weak_session_.lock())
    {
        return existing;
    }

    try
    {
        // buildConfig() returns a fresh config every time, and that is load
        // bearing: Session::open() moves out of whatever it is handed, leaving
        // it in zenoh's gravestone (null) state. Reusing one -- as a stored
        // static Config used to -- makes the *second* session creation reach an
        // unwrap_unchecked() inside z_config_loan_mut and abort the process.
        auto session = std::make_shared<zenoh::Session>(zenoh::Session::open(buildConfig()));
        weak_session_ = session;
        SPDLOG_DEBUG("Created new zenoh session.");
        return session;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to open zenoh session: {}", e.what());
        return {};
    }
}

void SessionManager::shutdown()
{
    std::lock_guard<std::mutex> lk(mutex_);
    weak_session_.reset();
}

} // namespace pub_sub


