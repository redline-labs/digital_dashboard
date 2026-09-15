// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loss-of-comm hook against a real publisher.
//
// The unit test pins the state machine; this pins the wiring around it: that
// the edges are detected on the delivery tick, on the GUI thread, that a widget
// bound to a live topic never goes stale while samples keep arriving, and that
// suppression silences the whole mechanism for a process (which is what the
// editor relies on to preview a layout with no bus behind it).
#include "dashboard/staleness.h"
#include "value_readout/value_readout.h"

#include "engine_rpm.capnp.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"

#include <QApplication>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
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

// Qt delivers the value on a timer, so the test has to let the event loop run.
void pump(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
}

ValueReadoutConfig_t configFor(const std::string& key, std::uint32_t stale_after_ms)
{
    ValueReadoutConfig_t config;
    config.zenoh_key = key;
    config.schema_type = pub_sub::schema_type_t::EngineRpm;
    config.value_expression = "rpm";
    config.stale_after_ms = stale_after_ms;
    return config;
}

void publish(pub_sub::ZenohPublisher<EngineRpm>& publisher, std::uint32_t rpm)
{
    publisher.fields().setRpm(rpm);
    publisher.put();
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    spdlog::set_level(spdlog::level::warn);
    QApplication app(argc, argv);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_WARN("SKIP: no zenoh session could be opened on this host");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string key = "test/dashboard_widgets/stale";

    {
        ValueReadoutWidget widget(configFor(key, 300));
        pub_sub::ZenohPublisher<EngineRpm> publisher(key);
        pump(300ms);  // let the pair match

        // Samples keep arriving: the widget must never report stale, which is
        // the failure that would have every gauge flickering on a healthy bus.
        bool went_stale = false;
        for (int i = 0; i < 20; ++i)
        {
            publish(publisher, static_cast<std::uint32_t>(1000 + i));
            pump(25ms);
            went_stale = went_stale || widget.isBindingStale("value");
        }
        check(!went_stale, "a binding fed every 25 ms never goes stale on a 300 ms timeout");

        // Stop, and it goes stale within the timeout plus a delivery tick.
        pump(400ms);
        check(widget.isBindingStale("value"), "and goes stale once the samples stop");
        check(widget.property("stale").toBool(), "with the property an agent can read");

        // Resume, and it comes back.
        publish(publisher, 2000);
        pump(100ms);
        check(!widget.isBindingStale("value"), "and comes back when a sample arrives");
    }

    {
        // What the editor does, so a preview with no bus behind it does not
        // draw every widget as dead.
        dashboard::staleness::setSuppressed(true);
        ValueReadoutWidget widget(configFor(key, 100));
        pump(400ms);
        check(!widget.isBindingStale("value"), "suppression keeps a silent binding fresh");
        dashboard::staleness::setSuppressed(false);
    }

    {
        // A binding that cannot be built at all -- an expression that does not
        // compile -- has no data and says so, rather than showing a zero.
        ValueReadoutConfig_t broken = configFor(key, 250);
        broken.value_expression = "this is not an expression";
        ValueReadoutWidget widget(broken);
        check(widget.isBindingStale("value"), "a binding that failed to build reports no data");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
