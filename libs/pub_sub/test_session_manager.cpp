// SPDX-License-Identifier: GPL-3.0-or-later
//
// SessionManager lifecycle: opening a session after a previous one has been
// fully released.
//
// This exists because that aborted the process. SessionManager kept a static
// zenoh::Config and passed it to Session::open(), which moves out of whatever
// it is handed -- so after the first session the static was left in zenoh's
// gravestone (null) state. The *second* getOrCreate() then touched it, reached
// an unwrap_unchecked() inside z_config_loan_mut (zenohc src/config.rs:51) and
// killed the process with a non-unwinding Rust panic:
//
//     unsafe precondition(s) violated: hint::unreachable_unchecked
//
// It surfaced as a dashboard crash on a misconfigured widget -- a subscriber
// that failed validation was discarded, dropping the session refcount to zero
// between two widgets -- but the config had nothing to do with it. Any
// release-then-reacquire did it, and shutdown() (documented as "useful for
// tests/shutdown") made it unconditional.
//
// These cases need a real zenoh session, so if the first one cannot be opened
// at all -- a build host with no usable network -- the suite reports that and
// skips rather than failing, since it would be testing the environment.
#include "pub_sub/session_manager.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

// A live session is shared, not rebuilt, while anyone still holds it.
void testSessionIsSharedWhileHeld()
{
    auto first = pub_sub::SessionManager::getOrCreate();
    auto second = pub_sub::SessionManager::getOrCreate();
    expect(first != nullptr, "a session opens");
    expect(first == second, "concurrent callers share one session");
}

// THE REGRESSION. Reaching the assertion at all is the result: before the fix
// the process aborted inside the second getOrCreate() and nothing after this
// line ran.
void testSessionCanBeReopenedAfterRelease()
{
    {
        auto first = pub_sub::SessionManager::getOrCreate();
        expect(first != nullptr, "first session opens");
    }  // last owner goes away -> the weak_ptr expires

    auto second = pub_sub::SessionManager::getOrCreate();
    expect(second != nullptr, "a session opens again after the previous one was released");
}

// shutdown() drops the manager's handle, so the next getOrCreate() must build a
// new session rather than a config that has already been consumed.
void testShutdownThenReopen()
{
    {
        auto held = pub_sub::SessionManager::getOrCreate();
        expect(held != nullptr, "session open before shutdown");
    }
    pub_sub::SessionManager::shutdown();

    auto reopened = pub_sub::SessionManager::getOrCreate();
    expect(reopened != nullptr, "a session opens after shutdown()");
}

// The ordering my fix depends on: defaults are applied first and caller
// overrides after, so an override actually wins. If zenoh ever made the first
// write win instead, insertConfig() would silently stop working -- this pins
// the assumption against the real Config rather than against our wrapper.
void testConfigOverridesAreLastWriteWins()
{
    zenoh::Config config = zenoh::Config::create_default();
    config.insert_json5("mode", "\"peer\"");
    config.insert_json5("mode", "\"client\"");
    expect(config.get("mode").find("client") != std::string::npos,
           "a later insert_json5 for the same key wins");
}

// A malformed override must not cost us the session: buildConfig() reports it
// and carries on. Runs last -- it leaves the bad setting in the manager's
// global list, so every later build would log it again.
void testBadOverrideDoesNotPreventSession()
{
    pub_sub::SessionManager::shutdown();
    pub_sub::SessionManager::insertConfig("this/is/not/a/config/key", "{not json5");

    auto session = pub_sub::SessionManager::getOrCreate();
    expect(session != nullptr, "a bad config override is skipped rather than losing the session");
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    // If zenoh cannot open a session here at all, none of the below is testing
    // this code. Say so plainly instead of reporting a failure.
    if (pub_sub::SessionManager::getOrCreate() == nullptr)
    {
        SPDLOG_WARN("SKIP: no zenoh session could be opened on this host; "
                    "SessionManager lifecycle not exercised");
        return 0;
    }
    pub_sub::SessionManager::shutdown();

    testSessionIsSharedWhileHeld();
    testSessionCanBeReopenedAfterRelease();
    testShutdownThenReopen();
    testConfigOverridesAreLastWriteWins();
    testBadOverrideDoesNotPreventSession();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} of {} assertion(s) failed", failures, checks);
        return 1;
    }
    SPDLOG_INFO("all {} session manager assertions passed", checks);
    return 0;
}
