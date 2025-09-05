#include "expression_parser/session_manager.h"
#include "spdlog/spdlog.h"
#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace expression_parser
{

std::mutex SessionManager::mutex_ = {};
std::weak_ptr<zenoh::Session> SessionManager::weak_session_ = {};
zenoh::Config SessionManager::zenoh_config_ = zenoh::Config::create_default();

void SessionManager::insertConfig(std::string key, std::string value)
{
    std::lock_guard<std::mutex> lk(mutex_);
    zenoh_config_.insert_json5(key, value);
}

std::shared_ptr<zenoh::Session> SessionManager::getOrCreate()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (auto existing = weak_session_.lock())
    {
        return existing;
    }

    //zenoh_config_.insert_json5("mode", "\"client\"");
    //zenoh_config_.insert_json5("connect/endpoints", "[\"tcp/localhost:7447\"]");

    try
    {
        auto session = std::make_shared<zenoh::Session>(zenoh::Session::open(std::move(zenoh_config_)));
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

} // namespace expression_parser


