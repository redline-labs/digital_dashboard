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

#include "pub_sub/node_identity.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/topic_directory.h"
#include "pub_sub/topic_key.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"

#include "can_bridge.capnp.h"
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

    // --------------------------------------------------------- topic ownership

    // The fifth segment: an advertisement now says WHO offers the topic, not
    // just what it is. Without it a tool can list every topic on the bus and
    // still only print opaque session ids beside them.
    {
        const std::string owned_key = "test/directory/owned";
        pub_sub::ZenohPublisher<EngineRpm> publisher(owned_key);

        expect(waitFor([&] { return find(directory, owned_key).has_value(); }),
               "an owned topic appears");

        const auto entry = find(directory, owned_key);
        expect(entry && !entry->owner_zid.empty(),
               "the advertisement carries the publisher's session id");
        expect(entry && entry->owner_zid == pub_sub::SessionManager::zid(),
               "and it is OUR session id -- the same value SessionManager reports, which is "
               "what makes the join against the node directory work");
    }

    // ------------------------------------------------------ the node directory

    {
        pub_sub::NodeDirectory nodes;
        if (!nodes.isValid())
        {
            std::fprintf(stderr, "WARNING: could not watch the node space; skipping node cases.\n");
        }
        else
        {
            const std::string zid = pub_sub::SessionManager::zid();

            // Nothing is announced for this process until it says so. A short
            // window on purpose: the claim is "no node appears promptly", and
            // waiting the full five seconds to assert an absence only makes the
            // suite slow without making the statement stronger.
            expect(!waitFor([&] { return !nodes.nameFor(zid).empty(); },
                            std::chrono::milliseconds(300)),
                   "no node is announced until one declares a NodeIdentity");

            {
                pub_sub::NodeIdentity identity("test_directory_node");
                expect(identity.isValid(), "the identity token is declared");
                expect(identity.zid() == zid,
                       "the identity reports the same session id as SessionManager");

                expect(waitFor([&] { return nodes.nameFor(zid) == "test_directory_node"; }),
                       "the node appears in the directory under its declared name");

                // The join that is the whole point: a topic's owner_zid resolves
                // to a readable name.
                const auto entry = find(directory, "test/directory/owned");
                expect(entry && nodes.nameFor(entry->owner_zid) == "test_directory_node",
                       "a topic's owner zid resolves to a node name -- the join the two "
                       "spaces exist to make possible");
            }

            // Same rule as topics: marked unreachable, never removed, so
            // "carplay was here and went away" stays visible.
            expect(waitFor([&] {
                       for (const pub_sub::NodeEntry& entry : nodes.snapshot())
                       {
                           if (entry.zid == zid && !entry.reachable)
                           {
                               return true;
                           }
                       }
                       return false;
                   }),
                   "a node that goes away is marked unreachable rather than dropped");

            expect(!nodes.nameFor(zid).empty(),
                   "and its name is still resolvable afterwards, so an old topic's owner "
                   "does not become anonymous when the node exits");
        }
    }

    // --------------------------------------------------- the service directory

    // Services were previously undiscoverable: zenoh would route a request to a
    // queryable, but nothing on the bus said it existed, what key to send to, or
    // what a request should contain. You had to read the source of whichever
    // node offers it.
    {
        pub_sub::ServiceDirectory services;
        if (!services.isValid())
        {
            std::fprintf(stderr,
                         "WARNING: could not watch the service space; skipping service cases.\n");
        }
        else
        {
            const std::string service_key = "test/directory/set_bitrate";

            const auto findService = [&services](const std::string& key)
                -> std::optional<pub_sub::ServiceEntry>
            {
                for (const pub_sub::ServiceEntry& entry : services.snapshot())
                {
                    if (entry.key == key)
                    {
                        return entry;
                    }
                }
                return std::nullopt;
            };

            {
                pub_sub::ZenohService<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse>
                    service(service_key,
                            [](const CanBridgeSetBitrateRequest::Reader&,
                               CanBridgeSetBitrateResponse::Builder& response)
                            { response.setOk(true); });

                expect(waitFor([&] { return findService(service_key).has_value(); }),
                       "a declared service advertises itself");

                const auto entry = findService(service_key);
                expect(entry && entry->request_schema == "CanBridgeSetBitrateRequest",
                       "the advertisement names the request schema, so a caller can build one");
                expect(entry && entry->response_schema == "CanBridgeSetBitrateResponse",
                       "and the response schema, so a caller can read the reply");
                expect(entry && entry->owner_zid == pub_sub::SessionManager::zid(),
                       "and the session offering it");
                expect(entry && entry->reachable, "and it is reachable while it is up");
            }

            expect(waitFor([&] {
                       const auto entry = findService(service_key);
                       return entry && !entry->reachable;
                   }),
                   "a service that goes away is marked unreachable rather than dropped");
        }
    }

    // A name that is not a usable key segment must be refused loudly rather than
    // producing a key every reader silently skips.
    {
        pub_sub::NodeIdentity bad("has/slash");
        expect(!bad.isValid(), "a node name containing '/' is refused");
    }
    {
        pub_sub::NodeIdentity bad("has@at");
        expect(!bad.isValid(), "a node name containing '@' is refused");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
