// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/table.h"

#include "node_health/codec.h"

#include "pub_sub/schema_registry.h"

#include <algorithm>

namespace node_health
{

HealthTable::HealthTable(ClassifyOptions options) : options_(options)
{
}

void HealthTable::identity(const std::string& zid, const std::string& name, bool reachable,
                           Clock::time_point now)
{
    if (zid.empty())
    {
        return;
    }
    const auto [it, inserted] = identities_.try_emplace(zid, Identity{name, reachable, now});
    Identity& known = it->second;
    if (inserted || known.name != name || known.reachable != reachable)
    {
        known.name = name;
        known.reachable = reachable;
        ++revision_;
    }
}

bool HealthTable::sample(std::string_view schema_name, const std::vector<std::uint8_t>& payload,
                         std::string_view origin_zid, Clock::time_point now)
{
    if (schema_name != pub_sub::schema_traits<::NodeHealth>::name)
    {
        return false;
    }
    std::optional<HealthSnapshot> snapshot = decodePayload(payload);
    if (!snapshot)
    {
        return false;
    }
    const std::string zid = snapshot->zid.empty() ? std::string(origin_zid) : snapshot->zid;
    if (zid.empty())
    {
        return false;
    }

    Record& record = records_[zid];
    account(record.continuity, record.last ? &*record.last : nullptr, *snapshot);
    record.last = std::move(snapshot);
    record.last_received = now;
    ++revision_;
    return true;
}

std::vector<HealthRow> HealthTable::rows(Clock::time_point now) const
{
    struct Joined
    {
        const Identity* identity = nullptr;
        const Record* record = nullptr;
    };
    std::map<std::string, Joined> joined;
    for (const auto& [zid, identity] : identities_)
    {
        joined[zid].identity = &identity;
    }
    for (const auto& [zid, record] : records_)
    {
        joined[zid].record = &record;
    }

    std::vector<HealthRow> out;
    out.reserve(joined.size());
    for (const auto& [zid, join] : joined)
    {
        Observation observation;
        if (join.identity != nullptr)
        {
            observation.identity_known = true;
            observation.identity_reachable = join.identity->reachable;
            observation.identity_first_seen = join.identity->first_seen;
        }
        if (join.record != nullptr)
        {
            observation.last = join.record->last;
            observation.last_received = join.record->last_received;
        }

        HealthRow row;
        row.zid = zid;
        if (join.identity != nullptr && !join.identity->name.empty())
        {
            row.name = join.identity->name;
        }
        else if (observation.last)
        {
            row.name = observation.last->node;
        }
        row.verdict = classify(observation, now, options_);
        row.last = observation.last;
        if (observation.last_received)
        {
            row.age = std::max(std::chrono::milliseconds(0),
                               std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - *observation.last_received));
        }
        if (join.record != nullptr)
        {
            row.continuity = join.record->continuity;
        }
        out.push_back(std::move(row));
    }

    std::sort(out.begin(), out.end(),
              [](const HealthRow& lhs, const HealthRow& rhs)
              {
                  if (lhs.name != rhs.name)
                  {
                      return lhs.name < rhs.name;
                  }
                  return lhs.zid < rhs.zid;
              });
    return out;
}

}  // namespace node_health
