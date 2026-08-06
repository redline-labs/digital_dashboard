#ifndef INSPECT_TRAFFIC_H_
#define INSPECT_TRAFFIC_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace inspect
{

// What was observed on one key over an interval.
//
// Everything here is derived from real arrival times, not from a sleep. The old
// `inspect hz` counted messages and printed the count once per
// `sleep_for(seconds(1))`, with the clock code commented out -- so what it
// actually reported was "messages per however long that sleep happened to take",
// which on a loaded machine is not one second. On a bus where the answer matters
// (is this publisher keeping up?) that is exactly when the number is worst.
struct KeyStats
{
    std::string key;
    std::string schema;

    // The session that sent them, from the sample timestamps. Empty when the
    // samples were unstamped; set to "(mixed)" when more than one session
    // published on this key during the interval -- which means two nodes are
    // publishing the same topic, and is worth seeing rather than averaging away.
    std::string origin_zid;

    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;

    // Wall time this interval actually covered, from a steady clock.
    double interval_seconds = 0.0;

    // messages / interval_seconds.
    double hz = 0.0;

    // bytes / interval_seconds.
    double bytes_per_second = 0.0;

    // How many inter-arrival gaps the period figures below are built from.
    //
    // NOT the same as `messages - 1`. A window carries in the arrival time of
    // the previous window's last message, so a 1 Hz topic delivering a single
    // message per second still yields one gap -- and reporting "too few for a
    // period" forever for exactly the slow publishers whose timing you most want
    // to see would be the wrong answer. Zero here is the only honest reason to
    // suppress the period columns.
    std::uint64_t period_samples = 0;

    // Inter-arrival gaps, in seconds. All zero when period_samples is zero.
    double period_mean = 0.0;
    double period_min = 0.0;
    double period_max = 0.0;
    double period_stddev = 0.0;

    // Arrival minus publish time, in seconds, over the samples that carried a
    // timestamp. `stamped` says how many did: a zero there means the numbers
    // below are meaningless, and a caller must say so rather than printing 0.0.
    std::uint64_t stamped = 0;
    double latency_mean = 0.0;
    double latency_min = 0.0;
    double latency_max = 0.0;
    double latency_p99 = 0.0;

    // Seconds since the last message on this key, at the moment of the report.
    double since_last = 0.0;
};

// Subscribes to a key expression and tabulates what arrives, per concrete key.
//
// One subscription however many keys match, which is why this exists rather than
// each verb opening its own: `watch` over a whole bus would otherwise need a
// subscription per topic, and the tree already learned that lesson in scope.
class TrafficMonitor
{
  public:
    explicit TrafficMonitor(const std::string& keyexpr);
    ~TrafficMonitor();

    TrafficMonitor(const TrafficMonitor&) = delete;
    TrafficMonitor& operator=(const TrafficMonitor&) = delete;

    // False when the subscription could not be declared.
    bool isValid() const;

    // Statistics for everything seen since the previous call to interval(),
    // then reset. The first call covers everything since construction.
    //
    // Keys with no traffic in the window are omitted, which is what makes this
    // usable for a per-second readout; `seen()` is there for a caller that needs
    // the full set.
    std::vector<KeyStats> interval();

    // Every key seen since construction, whether or not it had traffic in the
    // last window. Counts are cumulative.
    std::vector<KeyStats> cumulative() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace inspect

#endif  // INSPECT_TRAFFIC_H_
