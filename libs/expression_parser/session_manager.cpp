#include "expression_parser/session_manager.h"
#include "spdlog/spdlog.h"

namespace expression_parser {

std::mutex SessionManager::mutex_ = {};
std::weak_ptr<zenoh::Session> SessionManager::weak_session_ = {};
std::optional<zenoh::Config> SessionManager::default_config_ = std::nullopt;

void SessionManager::setDefaultConfig(zenoh::Config&& config)
{
    std::lock_guard<std::mutex> lk(mutex_);
    default_config_.emplace(std::move(config));
}

std::shared_ptr<zenoh::Session> SessionManager::getOrCreate()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (auto existing = weak_session_.lock()) {
        SPDLOG_INFO("Reusing existing zenoh session");
        return existing;
    }

    try {
        zenoh::Config config = default_config_.has_value()
            ? std::move(*default_config_)
            : zenoh::Config::create_default();
        auto session = std::make_shared<zenoh::Session>(zenoh::Session::open(std::move(config)));
        weak_session_ = session;
        SPDLOG_INFO("Opened shared zenoh session");
        return session;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to open zenoh session: {}", e.what());
        return {};
    }
}

void SessionManager::shutdown()
{
    std::lock_guard<std::mutex> lk(mutex_);
    weak_session_.reset();
    default_config_.reset();
}

} // namespace expression_parser


