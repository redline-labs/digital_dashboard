// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a sample carries besides its bytes, over a real bus.
//
// This is the test that proves timestamping is actually ON. The conversion
// arithmetic has its own unit test (test_timestamp.cpp); this one asserts the
// part no arithmetic can: that zenoh hands us a timestamp at all.
//
// It matters because the failure is invisible. Zenoh's default is
// `timestamping: { enabled: { router: true, peer: false, client: false } }` and
// every session in this tree is a peer -- so before SessionManager::buildConfig
// turned it on, Sample::get_timestamp() returned nullopt for every sample ever
// published here, forever, with no error anywhere. A consumer written against it
// would simply find no time and fall back, and everything downstream would look
// like it worked while quietly recording arrival times labelled as publish
// times.
//
// Mutation-check: delete the timestamping line from buildConfig() and
// testSamplesAreStamped must fail.
//
// `net`, and it skips itself where no session can be opened -- there is nothing
// of ours left to test on such a host.

#include "pub_sub/raw_subscriber.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/timestamp.h"
#include "pub_sub/zenoh_publisher.h"

#include "engine_rpm.capnp.h"
#include "vehicle_speed.capnp.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

std::uint64_t wallClockNanos()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// One captured sample. Views in SampleInfo borrow from the live zenoh sample, so
// everything is copied out here -- which is also the contract the header states,
// and getting it wrong would show up as garbage strings rather than a crash.
struct Captured
{
    std::string keyexpr;
    std::string schema_name;
    std::optional<std::uint64_t> publish_time_nanos;
    std::string origin_zid;
};

class Collector
{
  public:
    void add(const pub_sub::RawSubscriber::SampleInfo& info)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(Captured{
            std::string(info.keyexpr),
            std::string(info.schema_name),
            info.publish_time_nanos,
            std::string(info.origin_zid),
        });
    }

    std::vector<Captured> snapshot() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return samples_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Captured> samples_;
};

// The key this test publishes on.
//
// Deliberately distinctive rather than a realistic one like
// "vehicle/engine/rpm": the subscription below is a wildcard, so anything else
// publishing on this machine -- a mock publisher left running, another
// developer's node, a second copy of the suite -- lands in the same callback.
// Using a real key made this test pass or fail depending on what else happened
// to be up, which is worse than not having it.
constexpr const char* kTestKey = "test/sample_metadata/rpm";

// A second key, published on in the same run. Two keys through ONE wildcard
// subscription is what actually tests that SampleInfo::keyexpr means something:
// with a single key, "the subscriber reported the right key" is unfalsifiable,
// because there is only one right answer and any constant would pass.
constexpr const char* kTestKeyB = "test/sample_metadata/speed";

// Publishes `count` messages on kTestKey and returns what a wildcard subscriber
// saw OF THAT KEY.
//
// The subscription stays '**' because that is half of what is under test: a
// wildcard subscriber has to be told the concrete key of each sample, and the
// plain Handler cannot say. The filtering happens after collection so the test
// still exercises the wildcard path while asserting only about its own traffic.
//
// The subscriber is declared before the publisher and destroyed after the
// collector it writes into -- zenoh's undeclare joins in-flight callbacks, so
// this ordering is what makes reading the collector afterwards safe.
std::vector<Captured> publishAndCollect(int count)
{
    Collector collector;

    {
        pub_sub::RawSubscriber subscriber(
            "**", pub_sub::RawSubscriber::InfoHandler(
                      [&collector](const std::vector<std::uint8_t>&,
                                   const pub_sub::RawSubscriber::SampleInfo& info)
                      { collector.add(info); }));

        if (!subscriber.isValid())
        {
            SPDLOG_ERROR("could not declare the wildcard subscriber");
            return {};
        }

        // Zenoh discovery is not instantaneous even in-process; without this the
        // first sample or two are published before the subscription is matched
        // and the test reports "no samples" for a reason that has nothing to do
        // with what it is testing.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        pub_sub::ZenohPublisher<EngineRpm> rpm(kTestKey);
        pub_sub::ZenohPublisher<VehicleSpeed> speed(kTestKeyB);
        for (int i = 0; i < count; ++i)
        {
            rpm.fields().setRpm(1000.0F + static_cast<float>(i));
            rpm.put();
            speed.fields().setSpeedMps(10.0F + static_cast<float>(i));
            speed.put();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // Only our own traffic. Everything below asserts "every sample must ...",
    // and those claims are only true of samples this test produced. Another
    // process on this machine -- a mock publisher left running, a second copy of
    // the suite -- may legitimately be unstamped, on another schema, or from
    // another session, and none of that is this test's business.
    std::vector<Captured> mine;
    for (Captured& sample : collector.snapshot())
    {
        if (sample.keyexpr == kTestKey || sample.keyexpr == kTestKeyB)
        {
            mine.push_back(std::move(sample));
        }
    }
    return mine;
}

// ------------------------------------------------------------------ the cases

std::vector<Captured> g_samples;

void testSamplesArrive()
{
    expect(!g_samples.empty(), "a wildcard subscriber receives published samples");
}

// THE ONE THAT MATTERS. Every sample must be stamped -- not "most", not "at
// least one": a recorder that has to synthesise a time for some fraction of its
// messages is exactly the situation the fallback counting in bag exists to
// report, and here on a single host with one session there is no legitimate
// reason for any sample to lack a stamp.
void testSamplesAreStamped()
{
    if (g_samples.empty())
    {
        return;
    }

    std::size_t stamped = 0;
    for (const Captured& sample : g_samples)
    {
        if (sample.publish_time_nanos.has_value())
        {
            ++stamped;
        }
    }

    expect(stamped == g_samples.size(),
           "every sample carries a publish timestamp (" + std::to_string(stamped) + " of " +
               std::to_string(g_samples.size()) + ")");
}

// The stamp has to be a real wall-clock time, which is what catches a conversion
// that is off by a factor of 2^32 in a way no amount of arithmetic testing can:
// only a comparison against the actual clock knows what the magnitude should be.
//
// The window is deliberately loose -- a minute either side. It is not measuring
// clock accuracy, it is separating "now" from "1970" and "the year 2500".
void testStampIsNearWallClock()
{
    if (g_samples.empty())
    {
        return;
    }

    const std::uint64_t now = wallClockNanos();
    constexpr std::uint64_t kOneMinuteNanos = 60ull * 1'000'000'000ull;

    bool all_plausible = true;
    for (const Captured& sample : g_samples)
    {
        if (!sample.publish_time_nanos)
        {
            continue;
        }
        const std::uint64_t stamp = *sample.publish_time_nanos;
        const std::uint64_t difference = stamp > now ? stamp - now : now - stamp;
        if (difference > kOneMinuteNanos)
        {
            SPDLOG_ERROR("stamp {} is {} ns away from wall clock {}", stamp, difference, now);
            all_plausible = false;
        }
    }

    expect(all_plausible, "publish timestamps are within a minute of the wall clock");
}

// Monotonic within one session: the HLC guarantees it, and a bag's time index is
// built on the assumption. Both publishers here share this process's session, so
// the whole interleaved stream must be ordered, not just each key's.
void testStampsAreOrdered()
{
    if (g_samples.size() < 2u)
    {
        return;
    }

    bool ordered = true;
    std::uint64_t previous = 0u;
    for (const Captured& sample : g_samples)
    {
        if (!sample.publish_time_nanos)
        {
            continue;
        }
        if (*sample.publish_time_nanos < previous)
        {
            ordered = false;
        }
        previous = *sample.publish_time_nanos;
    }

    expect(ordered, "successive samples from one session have non-decreasing stamps");
}

// The origin zid is the point of stamping beyond the time itself: it says which
// session actually sent the bytes.
void testOriginZidIsPresentAndStable()
{
    if (g_samples.empty())
    {
        return;
    }

    bool all_present = true;
    bool all_same = true;
    const std::string& first = g_samples.front().origin_zid;

    for (const Captured& sample : g_samples)
    {
        if (sample.origin_zid.empty())
        {
            all_present = false;
        }
        if (sample.origin_zid != first)
        {
            all_same = false;
        }
    }

    expect(all_present, "every stamped sample names the session that sent it");
    expect(all_same, "all samples from one publisher report the same origin zid");
}

// A wildcard subscriber gets one callback per key and has to be told which. This
// is the reason SampleInfo exists at all -- the plain Handler cannot answer it,
// and a recorder subscribed to "**" cannot work without it.
//
// Two keys, so the assertion is falsifiable: with one key, "it reported the
// right key" would also pass for an implementation that returned a constant, or
// the subscription's own expression, or the last key it happened to see.
void testKeyDistinguishesTopics()
{
    if (g_samples.empty())
    {
        return;
    }

    std::size_t on_a = 0;
    std::size_t on_b = 0;
    for (const Captured& sample : g_samples)
    {
        if (sample.keyexpr == kTestKey)
        {
            ++on_a;
        }
        else if (sample.keyexpr == kTestKeyB)
        {
            ++on_b;
        }
    }

    expect(on_a > 0, "samples on the first key are reported under it");
    expect(on_b > 0, "samples on the second key are reported under it");
    expect(on_a + on_b == g_samples.size(),
           "every sample is attributed to exactly one of the two keys -- so the subscription's "
           "own '**' is never reported as the key");
}

// The schema name still rides alongside the new fields, and follows the key
// rather than being cached from the first sample seen.
void testSchemaNameFollowsTheKey()
{
    if (g_samples.empty())
    {
        return;
    }

    bool all_correct = true;
    for (const Captured& sample : g_samples)
    {
        const std::string_view expected =
            sample.keyexpr == kTestKey ? "EngineRpm" : "VehicleSpeed";
        if (sample.schema_name != expected)
        {
            SPDLOG_ERROR("on key '{}' expected schema '{}', got '{}'", sample.keyexpr, expected,
                         sample.schema_name);
            all_correct = false;
        }
    }

    expect(all_correct, "each sample's schema name matches the topic it arrived on");
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    if (pub_sub::SessionManager::getOrCreate() == nullptr)
    {
        SPDLOG_WARN("SKIP: no zenoh session could be opened on this host; "
                    "sample metadata not exercised");
        return 0;
    }

    g_samples = publishAndCollect(5);

    testSamplesArrive();
    testSamplesAreStamped();
    testStampIsNearWallClock();
    testStampsAreOrdered();
    testOriginZidIsPresentAndStable();
    testKeyDistinguishesTopics();
    testSchemaNameFollowsTheKey();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} of {} assertion(s) failed", failures, checks);
        return 1;
    }
    SPDLOG_INFO("all {} sample metadata assertions passed over {} samples", checks,
                g_samples.size());
    return 0;
}
