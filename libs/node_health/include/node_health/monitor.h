// SPDX-License-Identifier: GPL-3.0-or-later
//
// The aggregating side: every node's latest health, joined against which
// processes are alive.
#ifndef NODE_HEALTH_MONITOR_H_
#define NODE_HEALTH_MONITOR_H_

#include "node_health/classify.h"
#include "node_health/state.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

// Subscribes to `nodes/*/health` and watches the node directory. Rows are keyed
// by session id, so two instances of the same node are two rows.
class HealthMonitor
{
  public:
    explicit HealthMonitor(ClassifyOptions options = {});
    ~HealthMonitor();

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;
    HealthMonitor(HealthMonitor&&) = delete;
    HealthMonitor& operator=(HealthMonitor&&) = delete;

    // False when either subscription could not be declared.
    bool isValid() const;

    // Every node seen, sorted by name then session, classified at now. A copy,
    // safe from any thread.
    std::vector<HealthRow> snapshot() const;
    std::vector<HealthRow> snapshotAt(Clock::time_point now) const;

    // Moves when a sample arrives or the directory changes. It does NOT move as
    // time passes, and `late` is purely a function of time -- a consumer that
    // redraws only on a changed revision will never show a node going late.
    std::uint64_t revision() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace node_health

#endif  // NODE_HEALTH_MONITOR_H_
