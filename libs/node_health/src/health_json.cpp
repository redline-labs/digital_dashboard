// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/health_json.h"

#include <string>

namespace node_health
{
namespace
{

using nlohmann::json;

json checksJson(const HealthSnapshot& snapshot)
{
    json checks = json::array();
    for (const auto& check : snapshot.checks)
    {
        checks.push_back({
            {"name", check.name},
            {"state", std::string(to_string(check.state))},
            {"detail", check.detail},
        });
    }
    return checks;
}

}  // namespace

json healthRowJson(const HealthRow& row)
{
    json item{
        {"name", row.name},
        {"zid", row.zid},
        {"verdict", std::string(to_string(row.verdict))},
        // Whether a person needs to act, decided by the classifier rather than
        // by a browser guessing from the verdict string.
        {"healthy", isHealthy(row.verdict)},
        {"restarts", row.continuity.restarts},
        {"missed_samples", row.continuity.missed},
    };

    item["age_ms"] = row.age ? json(row.age->count()) : json(nullptr);

    if (row.last)
    {
        item["state"] = std::string(to_string(row.last->state));
        item["sequence"] = row.last->sequence;
        item["uptime_ms"] = row.last->uptime_ms;
        item["period_ms"] = row.last->period_ms;
        item["pid"] = row.last->pid;
        item["checks"] = checksJson(*row.last);
    }
    else
    {
        // Alive by identity but never reported: distinct from a node that is
        // gone, and the verdict already says which.
        item["state"] = nullptr;
        item["checks"] = json::array();
    }
    return item;
}

json healthReportJson(const std::vector<HealthRow>& rows, std::uint64_t revision)
{
    json nodes = json::array();
    for (const HealthRow& row : rows)
    {
        nodes.push_back(healthRowJson(row));
    }
    return json{{"bus_available", true}, {"nodes", nodes}, {"revision", revision}};
}

}  // namespace node_health
