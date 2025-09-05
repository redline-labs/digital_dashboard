#ifndef ZENOH_SESSION_MANAGER_H
#define ZENOH_SESSION_MANAGER_H

#include "zenoh.hxx"
#include <memory>
#include <mutex>
#include <optional>

namespace zenoh_session_manager
{

class SessionManager
{
  public:
    // Configure default Zenoh config to be used for the shared session
    static void insertConfig(std::string key, std::string value);

    // Get or create the shared Zenoh session (thread-safe)
    static std::shared_ptr<zenoh::Session> getOrCreate();

    // Close and reset the shared session (useful for tests/shutdown)
    static void shutdown();

  private:
    static std::mutex mutex_;
    static std::weak_ptr<zenoh::Session> weak_session_;
    static zenoh::Config zenoh_config_;
};

} // namespace zenoh_session_manager

#endif // ZENOH_SESSION_MANAGER_H


