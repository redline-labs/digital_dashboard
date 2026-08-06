#include "inspect/traffic.h"

#include "pub_sub/raw_subscriber.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

namespace inspect
{

namespace
{

using Clock = std::chrono::steady_clock;

double secondsBetween(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double>(to - from).count();
}

// What we keep per message. Deliberately small: at a few kHz across a whole bus
// this is allocated for every sample that arrives.
struct Sample
{
    Clock::time_point arrival;
    std::uint64_t bytes = 0;

    // Arrival minus publish, in seconds. Absent when the sample was unstamped.
    std::optional<double> latency;
};

struct KeyAccumulator
{
    std::string schema;

    // Empty until the first stamped sample; "(mixed)" once a second session is
    // seen. See KeyStats::origin_zid.
    std::string origin_zid;
    bool origin_conflict = false;

    std::vector<Sample> window;

    // Carried across intervals so hz is right for the FIRST message of a window
    // too -- without it, a 1 Hz topic reports its period as zero forever,
    // because each window contains exactly one message and a single message has
    // no gap.
    std::optional<Clock::time_point> last_arrival;

    std::uint64_t total_messages = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_stamped = 0;
    Clock::time_point first_arrival{};
};

// Nearest-rank percentile over a sorted range. For the small p99 samples here
// this is more honest than interpolating: it returns a latency that actually
// happened.
double percentile(const std::vector<double>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const std::size_t rank =
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, sorted.size() - 1);
    return sorted[index];
}

// Fills the statistical fields of `out` from `samples`, given the window's
// start and end and the arrival that preceded the window (if any).
void summarise(KeyStats& out, const std::vector<Sample>& samples,
               std::optional<Clock::time_point> previous_arrival, Clock::time_point window_start,
               Clock::time_point now)
{
    out.messages = samples.size();
    out.interval_seconds = secondsBetween(window_start, now);

    for (const Sample& sample : samples)
    {
        out.bytes += sample.bytes;
    }

    if (out.interval_seconds > 0.0)
    {
        out.hz = static_cast<double>(out.messages) / out.interval_seconds;
        out.bytes_per_second = static_cast<double>(out.bytes) / out.interval_seconds;
    }

    // Inter-arrival gaps. The gap from the previous window's last message counts
    // too, which is what makes a slow topic report a period at all.
    std::vector<double> periods;
    periods.reserve(samples.size());
    std::optional<Clock::time_point> previous = previous_arrival;
    for (const Sample& sample : samples)
    {
        if (previous)
        {
            periods.push_back(secondsBetween(*previous, sample.arrival));
        }
        previous = sample.arrival;
    }

    out.period_samples = periods.size();
    if (!periods.empty())
    {
        const auto [min_it, max_it] = std::minmax_element(periods.begin(), periods.end());
        out.period_min = *min_it;
        out.period_max = *max_it;

        double sum = 0.0;
        for (const double period : periods)
        {
            sum += period;
        }
        out.period_mean = sum / static_cast<double>(periods.size());

        double variance = 0.0;
        for (const double period : periods)
        {
            const double delta = period - out.period_mean;
            variance += delta * delta;
        }
        out.period_stddev = std::sqrt(variance / static_cast<double>(periods.size()));
    }

    // Latency, over the stamped samples only.
    std::vector<double> latencies;
    latencies.reserve(samples.size());
    for (const Sample& sample : samples)
    {
        if (sample.latency)
        {
            latencies.push_back(*sample.latency);
        }
    }

    out.stamped = latencies.size();
    if (!latencies.empty())
    {
        std::sort(latencies.begin(), latencies.end());
        out.latency_min = latencies.front();
        out.latency_max = latencies.back();

        double sum = 0.0;
        for (const double latency : latencies)
        {
            sum += latency;
        }
        out.latency_mean = sum / static_cast<double>(latencies.size());
        out.latency_p99 = percentile(latencies, 0.99);
    }

    if (previous)
    {
        out.since_last = secondsBetween(*previous, now);
    }
}

}  // namespace

struct TrafficMonitor::Impl
{
    mutable std::mutex mutex;
    std::map<std::string, KeyAccumulator> keys;
    Clock::time_point window_start = Clock::now();
    Clock::time_point started = Clock::now();

    // Declared last so it is destroyed FIRST: undeclaring joins in-flight
    // callbacks, so nothing can still be writing into the map above once it has
    // gone. The same rule as everywhere else that owns a subscription.
    std::unique_ptr<pub_sub::RawSubscriber> subscriber;
};

TrafficMonitor::TrafficMonitor(const std::string& keyexpr) : impl_(std::make_unique<Impl>())
{
    Impl* const impl = impl_.get();

    impl_->subscriber = std::make_unique<pub_sub::RawSubscriber>(
        keyexpr,
        pub_sub::RawSubscriber::InfoHandler(
            [impl](const std::vector<std::uint8_t>& payload,
                   const pub_sub::RawSubscriber::SampleInfo& info)
            {
                // Taken here rather than inside the lock: the clock read is the
                // measurement, and taking it after waiting for the mutex would
                // fold contention into the reported inter-arrival time.
                const Clock::time_point arrival = Clock::now();

                Sample sample;
                sample.arrival = arrival;
                sample.bytes = payload.size();

                if (info.publish_time_nanos)
                {
                    // Two different clocks: the stamp is the publisher's wall
                    // clock, and `arrival` is our steady clock. Subtracting them
                    // needs a wall-clock reading taken at the same moment.
                    const auto wall_now = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());

                    // Signed, because a publisher whose clock runs ahead of ours
                    // produces a stamp in our future and a negative latency.
                    // Reporting that honestly is better than clamping it to zero
                    // and calling the link instant.
                    const double latency =
                        (static_cast<double>(wall_now) -
                         static_cast<double>(*info.publish_time_nanos)) /
                        1e9;
                    sample.latency = latency;
                }

                const std::lock_guard<std::mutex> guard(impl->mutex);

                KeyAccumulator& accumulator = impl->keys[std::string(info.keyexpr)];
                if (accumulator.total_messages == 0)
                {
                    accumulator.first_arrival = arrival;
                }
                accumulator.schema = std::string(info.schema_name);

                if (!info.origin_zid.empty())
                {
                    if (accumulator.origin_zid.empty())
                    {
                        accumulator.origin_zid = std::string(info.origin_zid);
                    }
                    else if (accumulator.origin_zid != info.origin_zid)
                    {
                        accumulator.origin_conflict = true;
                    }
                }

                ++accumulator.total_messages;
                accumulator.total_bytes += sample.bytes;
                if (sample.latency)
                {
                    ++accumulator.total_stamped;
                }
                accumulator.window.push_back(sample);
            }));
}

TrafficMonitor::~TrafficMonitor() = default;

bool TrafficMonitor::isValid() const
{
    return impl_->subscriber && impl_->subscriber->isValid();
}

std::vector<KeyStats> TrafficMonitor::interval()
{
    const Clock::time_point now = Clock::now();

    std::vector<KeyStats> out;

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);

        for (auto& [key, accumulator] : impl_->keys)
        {
            if (accumulator.window.empty())
            {
                continue;
            }

            KeyStats stats;
            stats.key = key;
            stats.schema = accumulator.schema;
            stats.origin_zid =
                accumulator.origin_conflict ? "(mixed)" : accumulator.origin_zid;

            summarise(stats, accumulator.window, accumulator.last_arrival, impl_->window_start,
                      now);

            accumulator.last_arrival = accumulator.window.back().arrival;
            accumulator.window.clear();

            out.push_back(std::move(stats));
        }

        impl_->window_start = now;
    }

    return out;
}

std::vector<KeyStats> TrafficMonitor::cumulative() const
{
    const Clock::time_point now = Clock::now();

    std::vector<KeyStats> out;

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);

        for (const auto& [key, accumulator] : impl_->keys)
        {
            KeyStats stats;
            stats.key = key;
            stats.schema = accumulator.schema;
            stats.origin_zid =
                accumulator.origin_conflict ? "(mixed)" : accumulator.origin_zid;
            stats.messages = accumulator.total_messages;
            stats.bytes = accumulator.total_bytes;
            stats.stamped = accumulator.total_stamped;

            // Measured from this key's FIRST message rather than from when the
            // monitor started. A topic that only began publishing halfway
            // through would otherwise report half its actual rate.
            stats.interval_seconds = secondsBetween(accumulator.first_arrival, now);
            if (stats.interval_seconds > 0.0)
            {
                stats.hz = static_cast<double>(stats.messages) / stats.interval_seconds;
                stats.bytes_per_second =
                    static_cast<double>(stats.bytes) / stats.interval_seconds;
            }

            if (accumulator.last_arrival)
            {
                stats.since_last = secondsBetween(*accumulator.last_arrival, now);
            }
            else if (!accumulator.window.empty())
            {
                stats.since_last = secondsBetween(accumulator.window.back().arrival, now);
            }

            out.push_back(std::move(stats));
        }
    }

    return out;
}

}  // namespace inspect
