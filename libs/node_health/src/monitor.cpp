// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/monitor.h"

#include "pub_sub/raw_subscriber.h"
#include "pub_sub/topic_directory.h"

#include <atomic>
#include <mutex>

namespace node_health
{

struct HealthMonitor::Impl
{
    explicit Impl(ClassifyOptions classify_options)
        : started(Clock::now())
        , table(classify_options)
        , subscriber("nodes/*/health",
                     pub_sub::RawSubscriber::InfoHandler(
                         [this](const std::vector<std::uint8_t>& payload,
                                const pub_sub::RawSubscriber::SampleInfo& info)
                         { onSample(payload, info); }))
    {
    }

    void onSample(const std::vector<std::uint8_t>& payload, const pub_sub::RawSubscriber::SampleInfo& info)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (table.sample(info.schema_name, payload, info.origin_zid, Clock::now()))
        {
            revision.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Clock::time_point started;

    mutable std::mutex mutex;
    // Identities reach the table only when a snapshot polls the directory, so
    // mutable. Those already present at the first poll are dated from the
    // monitor's start: that is when it began listening for their health.
    mutable HealthTable table;
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
    for (const pub_sub::NodeEntry& identity : identities)
    {
        impl_->table.identity(identity.zid, identity.name, identity.reachable,
                              impl_->polled ? real_now : impl_->started);
    }
    impl_->polled = true;
    return impl_->table.rows(now);
}

std::uint64_t HealthMonitor::revision() const
{
    return impl_->revision.load(std::memory_order_relaxed) + impl_->directory.revision();
}

}  // namespace node_health
