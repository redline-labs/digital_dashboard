#ifndef EXPRESSION_PARSER_SESSION_MANAGER_H
#define EXPRESSION_PARSER_SESSION_MANAGER_H

#include "zenoh.hxx"
#include <memory>
#include <mutex>
#include <optional>

namespace expression_parser {

class SessionManager
{
  public:
    // Configure default Zenoh config to be used for the shared session
    static void setDefaultConfig(zenoh::Config&& config);

    // Get or create the shared Zenoh session (thread-safe)
    static std::shared_ptr<zenoh::Session> getOrCreate();

    // Close and reset the shared session (useful for tests/shutdown)
    static void shutdown();

  private:
    static std::mutex mutex_;
    static std::weak_ptr<zenoh::Session> weak_session_;
    static std::optional<zenoh::Config> default_config_;
};

} // namespace expression_parser

#endif // EXPRESSION_PARSER_SESSION_MANAGER_H


