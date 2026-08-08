#include "pub_sub/session_manager.h"
#include "spdlog/spdlog.h"
#include <condition_variable>
#include <cstdlib>
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

    // Stamp every sample we publish.
    //
    // Zenoh's default is `enabled: { router: true, peer: false, client: false }`
    // and every session here is a peer, so without this nothing on the bus
    // carries a time at all -- there is no publish time to record, no way to
    // measure transport latency, and a recorder or a plot can only use the
    // moment it happened to receive something.
    //
    // drop_future_timestamp stays false (the zenoh default): a sample stamped
    // ahead of local time is re-stamped rather than discarded. Dropping would be
    // worse here -- a unit whose RTC has not synced yet would vanish from the
    // bus entirely instead of arriving with a suspect time -- but it does mean
    // the time can be quietly wrong, which is why consumers record their own
    // arrival time alongside it. See pub_sub/timestamp.h.
    config.insert_json5("timestamping", "{\"enabled\": true, \"drop_future_timestamp\": false}");

    // PUB_SUB_NO_DISCOVERY=1 keeps this session off the machine's bus.
    //
    // Set for every test by cmake/ProjectTest.cmake, and it fixes a hang rather
    // than a preference. Two peers that have found each other and then close at
    // the same moment deadlock in zenoh's session teardown: each blocks in
    // z_session_drop waiting on the link to the other, which is itself blocked
    // waiting on the link back. One test process on its own always passed; two
    // of them started within a second of each other -- which is exactly what
    // `ctest -j8` does -- both hung until the 120 s timeout, and which test drew
    // the short straw varied by machine load. Reproduced at 4/4 processes hung,
    // 0/4 with this set.
    //
    // The deadlock is the loud version of a quieter problem the `net` label
    // already warns about: tests that find each other also SHARE A BUS, so one
    // test's samples arrive in another's subscriber. Nothing in the tree wants
    // that -- every test opens exactly one session through getOrCreate(), and
    // none of them needs to talk to another process.
    //
    // Applied before the caller's overrides, so a test that genuinely wants to
    // be found can still say so.
    if (const char* isolated = std::getenv("PUB_SUB_NO_DISCOVERY");
        isolated != nullptr && *isolated == '1')
    {
        config.insert_json5("scouting/multicast/enabled", "false");
    }

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

std::string SessionManager::zid()
{
    // Not cached. A zid belongs to a session, and shutdown() plus a later
    // getOrCreate() produces a different one -- caching would hand out the dead
    // session's id and attribute every topic to a node that no longer exists.
    // The call is a hex format of 16 bytes, so there is nothing here worth
    // caching anyway.
    const auto session = getOrCreate();
    if (!session)
    {
        return {};
    }

    try
    {
        return session->get_zid().to_string();
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to read the zenoh session id: {}", e.what());
        return {};
    }
}

void SessionManager::shutdown()
{
    std::lock_guard<std::mutex> lk(mutex_);
    weak_session_.reset();
}

} // namespace pub_sub


