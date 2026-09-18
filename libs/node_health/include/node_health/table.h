// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every node's latest health joined against its identity, and classified: the
// bookkeeping HealthMonitor does, without zenoh and without reading a clock.
//
// Its own class so that the web console's wasm module, which holds its own
// zenoh-pico session in the browser, runs the SAME joining, restart counting
// and classification as the node -- a verdict computed differently in the
// browser would be a second, quietly diverging opinion about the same bus.
#ifndef NODE_HEALTH_TABLE_H_
#define NODE_HEALTH_TABLE_H_

#include "node_health/classify.h"
#include "node_health/state.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace node_health
{

struct HealthRow
{
    std::string zid;
    // From the node's identity, or from its health sample if the directory has
    // not caught up yet.
    std::string name;
    Verdict verdict = Verdict::starting;
    std::optional<HealthSnapshot> last;
    // How long ago the last sample arrived.
    std::optional<std::chrono::milliseconds> age;
    Continuity continuity;
};

// Keyed by session id, so two instances of the same node are two rows. Not
// thread-safe; the owner locks.
class HealthTable
{
  public:
    explicit HealthTable(ClassifyOptions options = {});

    // A node identity (@redline/node/<zid>/<name>) seen at `now`, reachable or
    // gone. The first call for a zid dates it; later calls update the name and
    // reachability but never move that date, since silent is measured from it.
    void identity(const std::string& zid, const std::string& name, bool reachable,
                  Clock::time_point now);

    // A sample from nodes/*/health. Ignored, returning false, unless the
    // sample's schema is NodeHealth -- anything else under a health-shaped key
    // would decode into a plausible, wrong report -- and it decodes and names a
    // session, its own or else `origin_zid`.
    bool sample(std::string_view schema_name, const std::vector<std::uint8_t>& payload,
                std::string_view origin_zid, Clock::time_point now);

    // Every node seen, classified at `now`, sorted by name then session.
    std::vector<HealthRow> rows(Clock::time_point now) const;

    // Moves on every accepted sample and every identity change. Not with time:
    // `late` is a function of time alone.
    std::uint64_t revision() const { return revision_; }

  private:
    struct Identity
    {
        std::string name;
        bool reachable = false;
        Clock::time_point first_seen;
    };
    struct Record
    {
        std::optional<HealthSnapshot> last;
        std::optional<Clock::time_point> last_received;
        Continuity continuity;
    };

    ClassifyOptions options_;
    std::map<std::string, Identity> identities_;
    std::map<std::string, Record> records_;
    std::uint64_t revision_ = 0;
};

}  // namespace node_health

#endif  // NODE_HEALTH_TABLE_H_
