// SPDX-License-Identifier: GPL-3.0-or-later
//
// LiveZenohSource refuses a stream published as a different schema.
//
// It once passed the bare schema name to a check that expected the whole
// encoding string; the bare name looked like "no schema named", so a mismatch
// was logged at debug level and every sample decoded into a plausible wrong
// number. The assertion is on the buffer: nothing from the mismatched key may
// arrive, while the same publish shape on a matching key does -- which is what
// proves the bus was delivering at all.

#include "scope/live_zenoh_source.h"
#include "scope/sample_ring.h"

#include "pub_sub/zenoh_publisher.h"

#include <engine_rpm.capnp.h>
#include <vehicle_speed.capnp.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace
{

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::shared_ptr<scope::SignalBuffer> makeBuffer()
{
    return std::make_shared<scope::SignalBuffer>(60.0, 10000, 4096);
}

scope::SignalKey rpmOn(const std::string& key)
{
    scope::SignalKey signal;
    signal.zenoh_key = key;
    signal.schema_type = pub_sub::schema_type_t::EngineRpm;
    signal.expression = "rpm";
    return signal;
}

}  // namespace

int main()
{
    using namespace std::chrono_literals;

    const std::string base = "test/scope/live_source/" + std::to_string(::getpid());
    const std::string wrong_key = base + "/wrong";
    const std::string right_key = base + "/right";

    scope::LiveZenohSource source;
    const auto wrong = makeBuffer();
    const auto right = makeBuffer();
    expect(source.bind(rpmOn(wrong_key), wrong) != scope::kInvalidSignal, "binds the wrong key");
    expect(source.bind(rpmOn(right_key), right) != scope::kInvalidSignal, "binds the right key");

    pub_sub::ZenohPublisher<VehicleSpeed> speed(wrong_key);
    pub_sub::ZenohPublisher<EngineRpm> rpm(right_key);

    // Until the matching key has delivered, so the bus is known to be up before
    // the absence on the other one means anything.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        speed.fields().setSpeedMps(27.0f);
        speed.put();
        rpm.fields().setRpm(3000);
        rpm.put();
        std::this_thread::sleep_for(20ms);
        right->drain(source.now());
        if (right->history().size() >= 5)
        {
            break;
        }
    }
    // Another round after, so samples on the wrong key are not merely in flight.
    for (int i = 0; i < 5; ++i)
    {
        speed.fields().setSpeedMps(27.0f);
        speed.put();
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(200ms);
    wrong->drain(source.now());
    right->drain(source.now());

    expect(right->history().size() >= 5, "the matching schema is plotted");
    expect(wrong->history().size() == 0, "a stream published as another schema is not");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
