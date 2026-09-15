// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/monitor.h"

#include "node_health/codec.h"

#include "pub_sub/raw_subscriber.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/topic_directory.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace node_health
{

namespace
{

struct Record
{
    std::optional<HealthSnapshot> last;
    std::optional<Clock::time_point> last_received;
    Continuity continuity;
};

}  // namespace

struct HealthMonitor::Impl
{
    explicit Impl(ClassifyOptions classify_options)
        : options(classify_options)
        , started(Clock::now())
        , subscriber("nodes/*/health",
                     pub_sub::RawSubscriber::InfoHandler(
                         [this](const std::vector<std::uint8_t>& payload,
                                const pub_sub::RawSubscriber::SampleInfo& info)
                         { onSample(payload, info); }))
    {
    }

    void onSample(const std::vector<std::uint8_t>& payload, const pub_sub::RawSubscriber::SampleInfo& info)
    {
        // Something else published under a health-shaped key. Decoding it as
        // NodeHealth anyway would not throw -- it would produce a plausible,
        // wrong report.
        if (info.schema_name != pub_sub::schema_traits<::NodeHealth>::name)
        {
            return;
        }
        std::optional<HealthSnapshot> snapshot = decodePayload(payload);
        if (!snapshot)
        {
            return;
        }
        std::string zid = snapshot->zid.empty() ? std::string(info.origin_zid) : snapshot->zid;
        if (zid.empty())
        {
            return;
        }

        const Clock::time_point now = Clock::now();
        const std::lock_guard<std::mutex> lock(mutex);
        Record& record = records[zid];
        account(record.continuity, record.last ? &*record.last : nullptr, *snapshot);
        record.last = std::move(snapshot);
        record.last_received = now;
        revision.fetch_add(1, std::memory_order_relaxed);
    }

    ClassifyOptions options;
    Clock::time_point started;

    mutable std::mutex mutex;
    std::map<std::string, Record> records;
    // When this monitor first saw each identity. Identities already present
    // at the first snapshot are dated from the monitor's start: that is when it
    // began listening for their health.
    mutable std::map<std::string, Clock::time_point> first_seen;
    mutable bool polled = false;
    std::atomic<std::uint64_t> revision{0};

    pub_sub::NodeDirectory directory;
    // Last, so no sample can arrive before the members it writes exist.
    pub_sub::RawSubscriber subscriber;
};

HealthMonitor::HealthMonitor(ClassifyOptions options)
    : impl_(std::make_unique<Impl>(options))
{
}

HealthMonitor::~HealthMonitor() = default;

bool HealthMonitor::isValid() const
{
    return impl_->directory.isValid() && impl_->subscriber.isValid();
}

std::vector<HealthRow> HealthMonitor::snapshot() const
{
    return snapshotAt(Clock::now());
}

std::vector<HealthRow> HealthMonitor::snapshotAt(Clock::time_point now) const
{
    const std::vector<pub_sub::NodeEntry> identities = impl_->directory.snapshot();
    const Clock::time_point real_now = Clock::now();

    const std::lock_guard<std::mutex> lock(impl_->mutex);

    struct Joined
    {
        const pub_sub::NodeEntry* identity = nullptr;
        const Record* record = nullptr;
    };
    std::map<std::string, Joined> joined;
    for (const pub_sub::NodeEntry& identity : identities)
    {
        joined[identity.zid].identity = &identity;
        impl_->first_seen.try_emplace(identity.zid, impl_->polled ? real_now : impl_->started);
    }
    impl_->polled = true;
    for (const auto& [zid, record] : impl_->records)
    {
        joined[zid].record = &record;
    }

    std::vector<HealthRow> rows;
    rows.reserve(joined.size());
    for (const auto& [zid, join] : joined)
    {
        Observation observation;
        if (join.identity != nullptr)
        {
            observation.identity_known = true;
            observation.identity_reachable = join.identity->reachable;
            if (const auto it = impl_->first_seen.find(zid); it != impl_->first_seen.end())
            {
                observation.identity_first_seen = it->second;
            }
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
        row.verdict = classify(observation, now, impl_->options);
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
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(),
              [](const HealthRow& lhs, const HealthRow& rhs)
              {
                  if (lhs.name != rhs.name)
                  {
                      return lhs.name < rhs.name;
                  }
                  return lhs.zid < rhs.zid;
              });
    return rows;
}

std::uint64_t HealthMonitor::revision() const
{
    return impl_->revision.load(std::memory_order_relaxed) + impl_->directory.revision();
}

}  // namespace node_health
