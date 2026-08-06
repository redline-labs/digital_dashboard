// SPDX-License-Identifier: GPL-3.0-or-later
//
// Discovery via liveliness, end to end against a real session.
//
// The claim being tested is the one that motivated the whole mechanism: a topic
// appears in the directory as soon as a publisher exists, WITHOUT that publisher
// ever sending a sample. Traffic-observing discovery cannot do that -- a topic
// that has said nothing is indistinguishable from one that does not exist -- so
// this asserts the difference directly by never calling put().
//
// Needs a session, so `net`, and it skips itself when one cannot be opened,
// the same way the SessionManager test does.

#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/topic_directory.h"
#include "pub_sub/topic_key.h"
#include "pub_sub/zenoh_publisher.h"

#include "engine_rpm.capnp.h"
#include "vehicle_speed.capnp.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

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
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// Liveliness propagates through the session rather than instantly, so every
// assertion waits for a condition rather than sleeping a fixed amount and
// hoping. A fixed sleep is what makes this kind of test flaky on a loaded
// machine.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

std::optional<pub_sub::DirectoryEntry> find(const pub_sub::TopicDirectory& directory,
                                            const std::string& key)
{
    for (const pub_sub::DirectoryEntry& entry : directory.snapshot())
    {
        if (entry.key == key)
        {
            return entry;
        }
    }
    return std::nullopt;
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "WARNING: no zenoh session available; skipping.\n");
        return 0;
    }

    pub_sub::TopicDirectory directory;
    if (!directory.isValid())
    {
        std::fprintf(stderr, "WARNING: could not watch the advertisement space; skipping.\n");
        return 0;
    }

    const std::string rpm_key = "test/directory/rpm";
    const std::string speed_key = "test/directory/speed";

    // ---------------------------------------------------------- the main claim

    {
        pub_sub::ZenohPublisher<EngineRpm> publisher(rpm_key);
        expect(publisher.isValid(), "the publisher came up");

        expect(waitFor([&] { return find(directory, rpm_key).has_value(); }),
               "a topic appears in the directory from the publisher alone, with no sample "
               "ever published -- which is the whole point of advertising");

        const auto entry = find(directory, rpm_key);
        if (entry)
        {
            expect(entry->schema == "EngineRpm",
                   "the schema arrives with the advertisement, so fields are known before "
                   "any traffic");
            expect(entry->reachable, "a live publisher is reachable");
            expect(entry->appearances >= 1, "the appearance was counted");
        }

        // A second publisher on a different key, to check they do not collide.
        pub_sub::ZenohPublisher<VehicleSpeed> speed(speed_key);
        expect(waitFor([&] { return find(directory, speed_key).has_value(); }),
               "a second topic appears independently");

        const auto speed_entry = find(directory, speed_key);
        expect(speed_entry && speed_entry->schema == "VehicleSpeed",
               "each advertisement carries its own schema");
    }

    // ------------------------------------------------- what happens on teardown

    // Both publishers are now destroyed, which undeclares their tokens.
    expect(waitFor([&] {
               const auto entry = find(directory, rpm_key);
               return entry && !entry->reachable;
           }),
           "a destroyed publisher marks its topic unreachable");

    {
        const auto entry = find(directory, rpm_key);
        expect(entry.has_value(),
               "the entry is NOT removed -- a picker must not evict a row the user may have "
               "bound, and a DELETE only means unreachable from here");
        if (entry)
        {
            expect(entry->disappearances >= 1, "the disappearance was counted");
        }
    }

    // --------------------------------------------------------------- rebinding

    {
        pub_sub::ZenohPublisher<EngineRpm> republisher(rpm_key);
        expect(waitFor([&] {
                   const auto entry = find(directory, rpm_key);
                   return entry && entry->reachable;
               }),
               "a topic coming back is marked reachable again rather than duplicated");

        const auto entry = find(directory, rpm_key);
        expect(entry && entry->appearances >= 2,
               "appearances accumulate, so a consumer can tell a flapping topic from a "
               "stable one");

        const auto snapshot = directory.snapshot();
        const auto matching = std::count_if(
            snapshot.begin(), snapshot.end(),
            [&](const pub_sub::DirectoryEntry& e) { return e.key == rpm_key; });
        expect(matching == 1, "a topic appears exactly once however often it comes and goes");
    }

    // ------------------------------------------------------------ housekeeping

    expect(directory.revision() > 0, "the revision moves, so a consumer can skip idle polls");

    // An invalid key must never reach the bus, so it must never be advertised.
    {
        pub_sub::ZenohPublisher<EngineRpm> bad("test/directory/bad@key");
        expect(!bad.isValid(), "a publisher on a key containing '@' refuses to come up");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        expect(!find(directory, "test/directory/bad@key").has_value(),
               "a refused publisher advertises nothing");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
