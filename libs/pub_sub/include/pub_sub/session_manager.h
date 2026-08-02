#ifndef ZENOH_SESSION_MANAGER_H
#define ZENOH_SESSION_MANAGER_H

#include "zenoh.hxx"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pub_sub
{

class SessionManager
{
  public:
    // Settings applied to every session this manager opens, on top of the
    // built-in defaults. Applied in the order inserted, so a later call to the
    // same key wins. May be called at any time; it affects the next session
    // opened, not one that is already running.
    static void insertConfig(std::string key, std::string value);

    // Get or create the shared Zenoh session (thread-safe)
    static std::shared_ptr<zenoh::Session> getOrCreate();

    // Close and reset the shared session (useful for tests/shutdown)
    static void shutdown();

  private:
    static std::mutex mutex_;
    static std::weak_ptr<zenoh::Session> weak_session_;

    // Deliberately the *settings*, not a zenoh::Config. Session::open() takes
    // its config by value and moves out of it, which leaves a stored Config in
    // zenoh's gravestone (null) state; touching one again -- as the second
    // getOrCreate() did -- reaches an unwrap_unchecked() inside
    // z_config_loan_mut and aborts the process. Keeping the recipe rather than
    // the object means every session gets a freshly built config and there is
    // nothing to consume.
    static std::vector<std::pair<std::string, std::string>> config_overrides_;

    // Builds a fresh config from the defaults plus config_overrides_.
    // Caller must hold mutex_.
    static zenoh::Config buildConfig();
};

} // namespace pub_sub

#endif // ZENOH_SESSION_MANAGER_H


