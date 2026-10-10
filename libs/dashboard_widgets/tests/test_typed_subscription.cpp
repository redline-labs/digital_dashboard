// SPDX-License-Identifier: GPL-3.0-or-later
//
// dashboard::TypedSubscription against a real bus: what reaches the widget, and
// what must not.
//
// The schema-name check is the case with history. ZenohTypedSubscriber checks
// only the layout revision, which an unstamped sample passes, and a message of
// another schema decodes silently into plausible wrong values; two widgets
// carried their own name check because of it. Here it is checked once, and
// this is what notices if it goes.

#include "dashboard/typed_subscription.h"

#include "engine_rpm.capnp.h"
#include "pub_sub/detail/byte_publisher.h"
#include "pub_sub/schema_layout.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"
#include "vehicle_speed.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <QApplication>
#include <QWidget>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void pump(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
}

struct Reading
{
    float speed = 0.0f;
    int extracted_so_far = 0;
};

class Receiver : public QWidget
{
  public:
    void set(Reading reading)
    {
        last = reading;
        ++deliveries;
    }
    Reading last;
    int deliveries = 0;
};

using Subscription = dashboard::TypedSubscription<VehicleSpeed, Reading>;

std::unique_ptr<Subscription> subscribe(const std::string& key, Receiver& receiver,
                                        std::chrono::milliseconds stale_after)
{
    return std::make_unique<Subscription>(
        key,
        // Counts every message it sees: state kept by the extractor between
        // calls survives coalescing, which is what album-art caching relies on.
        [count = 0](VehicleSpeed::Reader reader) mutable
        { return std::optional<Reading>(Reading{reader.getSpeedMps(), ++count}); },
        [&receiver](Reading reading) { receiver.set(reading); }, stale_after,
        [&receiver] { receiver.update(); });
}

void testABurstIsOneDeliveryOfTheLatest()
{
    const std::string key = "test/dashboard_widgets/typed/burst";
    Receiver receiver;
    auto subscription = subscribe(key, receiver, 0ms);
    pub_sub::ZenohPublisher<VehicleSpeed> publisher(key);
    pump(300ms);

    for (int i = 1; i <= 50; ++i)
    {
        publisher.fields().setSpeedMps(static_cast<float>(i));
        publisher.put();
    }
    pump(200ms);

    check(receiver.last.speed == 50.0f, "the latest value of a burst is what arrives");
    check(receiver.deliveries < 50, "and a burst is coalesced, not delivered one by one");
    check(receiver.last.extracted_so_far == 50,
          "the extractor saw every message and kept its own count across them");
}

void testAnotherSchemaOnTheKeyIsDropped()
{
    const std::string key = "test/dashboard_widgets/typed/wrong_schema";
    Receiver receiver;
    auto subscription = subscribe(key, receiver, 0ms);

    // Unstamped -- no layout revision -- which is what slips past a layout
    // check: only the stamped schema NAME says this is not a VehicleSpeed.
    pub_sub::detail::BytePublisher unstamped(key, pub_sub::schema_traits<EngineRpm>::name,
                                             pub_sub::kNoLayout);
    pump(300ms);

    capnp::MallocMessageBuilder message;
    message.initRoot<EngineRpm>().setRpm(4000);
    const kj::Array<capnp::word> flat = capnp::messageToFlatArray(message);
    auto words = kj::heapArray<capnp::word>(flat.size());
    std::memcpy(words.begin(), flat.begin(), flat.size() * sizeof(capnp::word));
    unstamped.put(std::move(words));
    pump(200ms);
    check(receiver.deliveries == 0,
          "an unstamped message named as another schema never reaches the widget");
}

void testAMalformedPayloadIsDropped()
{
    const std::string key = "test/dashboard_widgets/typed/malformed";
    Receiver receiver;
    auto subscription = subscribe(key, receiver, 0ms);
    pub_sub::detail::BytePublisher raw(key, pub_sub::schema_traits<VehicleSpeed>::name,
                                       pub_sub::schema_traits<VehicleSpeed>::layout);
    pump(300ms);

    // A segment table claiming four billion segments: capnp throws reading it.
    auto words = kj::heapArray<capnp::word>(1);
    std::memset(words.begin(), 0xFF, sizeof(capnp::word));
    raw.put(std::move(words));
    pump(200ms);
    check(receiver.deliveries == 0, "a malformed message is dropped, and nothing threw");
}

void testStalenessLandsAtItsDeadline()
{
    const std::string key = "test/dashboard_widgets/typed/stale";
    Receiver receiver;
    auto subscription = subscribe(key, receiver, 150ms);
    pub_sub::ZenohPublisher<VehicleSpeed> publisher(key);
    pump(300ms);

    publisher.fields().setSpeedMps(1.0f);
    publisher.put();
    pump(50ms);
    check(!subscription->isStale(), "fresh just after a message");
    pump(300ms);
    check(subscription->isStale(), "stale once the timeout passes with nothing new");
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    spdlog::set_level(spdlog::level::warn);
    QApplication app(argc, argv);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    testABurstIsOneDeliveryOfTheLatest();
    testAnotherSchemaOnTheKeyIsDropped();
    testAMalformedPayloadIsDropped();
    testStalenessLandsAtItsDeadline();

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
