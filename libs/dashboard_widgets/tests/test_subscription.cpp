// SPDX-License-Identifier: GPL-3.0-or-later
//
// pub_sub::subscription_t into dashboard::makeExpressionSubscription: what an
// unbound, a misconfigured and a working binding each turn into.
//
// The unbound case is the one with history. Every widget nobody had wired up
// used to log two errors at startup -- an empty key, then an "invalid
// expression" -- so a fresh layout opened to a wall of red about nothing.
// Unbound now means: no subscription, no log, never stale.

#include "dashboard/expression_subscription.h"

#include "config_codec/config_apply_limits.h"
#include "engine_rpm.capnp.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/subscription.h"
#include "pub_sub/zenoh_publisher.h"

#include <QApplication>
#include <QWidget>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
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

// Stands in for a widget: makeExpressionSubscription needs a receiver with
// update() and a setter.
class Receiver : public QWidget
{
  public:
    void setValue(double value) { last = value; ++deliveries; }
    double last = 0.0;
    int deliveries = 0;
};

std::atomic<int> g_logged_errors{0};

void testUnboundIsSilentAndNeverStale()
{
    Receiver receiver;
    pub_sub::subscription_t unbound;
    unbound.expression = "rpm";
    unbound.stale_after_ms = 100;

    const int errors_before = g_logged_errors.load();
    auto subscription = dashboard::makeExpressionSubscription<double>(unbound, &receiver,
                                                                      &Receiver::setValue);
    check(subscription == nullptr, "an unbound subscription is no subscription");
    check(g_logged_errors.load() == errors_before, "and says nothing about it");
    pump(200ms);
    check(!dashboard::isStale(subscription), "and is never stale");
}

void testAKeyWithNoExpressionGoesStale()
{
    Receiver receiver;
    pub_sub::subscription_t broken;
    broken.zenoh_key = "test/dashboard_widgets/subscription/broken";
    broken.schema_type = pub_sub::schema_type_t::EngineRpm;
    broken.stale_after_ms = 100;

    auto subscription = dashboard::makeExpressionSubscription<double>(broken, &receiver,
                                                                      &Receiver::setValue);
    check(subscription != nullptr, "a key with no expression is still subscribed");
    check(subscription && !subscription->isValid(), "but cannot deliver");
    pump(300ms);
    check(dashboard::isStale(subscription),
          "so it goes stale -- a gauge reading zero for a broken binding is worse than none");
}

void testEveryFieldReachesTheSubscriber(const std::string& key)
{
    Receiver receiver;
    pub_sub::subscription_t binding = pub_sub::subscriptionFor(pub_sub::schema_type_t::EngineRpm);
    binding.zenoh_key = key;
    binding.expression = "rpm / 2";
    binding.stale_after_ms = 150;

    auto subscription = dashboard::makeExpressionSubscription<double>(binding, &receiver,
                                                                      &Receiver::setValue);
    pub_sub::ZenohPublisher<EngineRpm> publisher(key);
    pump(300ms);  // let the pair match

    publisher.fields().setRpm(3000);
    publisher.put();
    pump(100ms);
    check(receiver.deliveries > 0 && receiver.last == 1500.0,
          "key, schema and expression all reached the subscriber");
    check(!dashboard::isStale(subscription), "fresh while it is being fed");

    pump(400ms);
    check(dashboard::isStale(subscription), "and stale_after_ms did too");
}

void testStaleAfterIsClampedWhereverTheStructSits()
{
    pub_sub::subscription_t too_short;
    too_short.stale_after_ms = 3;
    const auto notes = config_codec::applyLimits(too_short);
    check(too_short.stale_after_ms == config_codec::limits::kMinStaleAfterMs && notes.size() == 1,
          "a timeout inside three delivery ticks is clamped, and said");

    pub_sub::subscription_t never;
    check(config_codec::applyLimits(never).empty() && never.stale_after_ms == 0,
          "zero still means never, unremarked");

    pub_sub::subscription_t keyed;
    keyed.zenoh_key = "vehicle/speed";
    check(config_codec::applyLimits(keyed).size() == 1, "a key with no expression is remarked on");
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg& msg)
    {
        if (msg.level >= spdlog::level::err)
        {
            ++g_logged_errors;
        }
    });
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("capture", sink));
    QApplication app(argc, argv);

    testStaleAfterIsClampedWhereverTheStructSits();
    testUnboundIsSilentAndNeverStale();

    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    testAKeyWithNoExpressionGoesStale();
    testEveryFieldReachesTheSubscriber("test/dashboard_widgets/subscription/live");

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
