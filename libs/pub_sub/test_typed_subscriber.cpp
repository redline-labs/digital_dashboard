// SPDX-License-Identifier: GPL-3.0-or-later
//
// ZenohTypedSubscriber hands its callback only what it can trust.
//
// It checked the schema revision but never the schema NAME, so an unstamped
// sample of another schema decoded as this one -- plausible wrong values, no
// error. Each case publishes on its own key (the name verdict latches per
// subscriber) with a publisher that stamps exactly what the case needs.
#include "pub_sub/detail/byte_publisher.h"
#include "pub_sub/schema_layout.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include "engine_rpm.capnp.h"
#include "vehicle_speed.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>

namespace
{

using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string keyFor(const char* name)
{
    return "test/pub_sub/typed_subscriber/" + std::to_string(::getpid()) + "/" + name;
}

kj::Array<capnp::word> rpmMessage(std::uint32_t rpm)
{
    capnp::MallocMessageBuilder message;
    message.initRoot<EngineRpm>().setRpm(rpm);
    const kj::Array<capnp::word> flat = capnp::messageToFlatArray(message);
    auto words = kj::heapArray<capnp::word>(flat.size());
    std::memcpy(words.begin(), flat.begin(), flat.size() * sizeof(capnp::word));
    return words;
}

// How many messages a VehicleSpeed subscriber on `key` delivers while `publish`
// runs a few times.
template <typename Publish>
int deliveredWhile(const std::string& key, Publish publish)
{
    std::atomic<int> delivered{0};
    pub_sub::ZenohTypedSubscriber<VehicleSpeed> subscriber(
        key, [&](VehicleSpeed::Reader reader) {
            (void)reader.getSpeedMps();
            delivered.fetch_add(1);
        });
    std::this_thread::sleep_for(300ms);
    for (int i = 0; i < 5; ++i)
    {
        publish();
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(200ms);
    return delivered.load();
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    {
        const std::string key = keyFor("right");
        pub_sub::ZenohPublisher<VehicleSpeed> publisher(key);
        const int n = deliveredWhile(key, [&] {
            publisher.fields().setSpeedMps(27.0f);
            publisher.put();
        });
        expect(n == 5, "the expected schema is delivered (got " + std::to_string(n) + ")");
    }
    {
        // Unstamped: only the name says this is not a VehicleSpeed.
        const std::string key = keyFor("wrong_name");
        pub_sub::detail::BytePublisher publisher(key, pub_sub::schema_traits<EngineRpm>::name,
                                                 pub_sub::kNoLayout);
        const int n = deliveredWhile(key, [&] { publisher.put(rpmMessage(4000)); });
        expect(n == 0, "another schema's messages are dropped");
    }
    {
        const std::string key = keyFor("wrong_layout");
        pub_sub::detail::BytePublisher publisher(key, pub_sub::schema_traits<VehicleSpeed>::name,
                                                 pub_sub::schema_traits<VehicleSpeed>::layout + 1);
        const int n = deliveredWhile(key, [&] { publisher.put(rpmMessage(4000)); });
        expect(n == 0, "another revision of the schema is dropped");
    }
    {
        const std::string key = keyFor("malformed");
        pub_sub::detail::BytePublisher publisher(key, pub_sub::schema_traits<VehicleSpeed>::name,
                                                 pub_sub::schema_traits<VehicleSpeed>::layout);
        const int n = deliveredWhile(key, [&] {
            // A segment table claiming four billion segments: capnp throws.
            auto words = kj::heapArray<capnp::word>(1);
            std::memset(words.begin(), 0xFF, sizeof(capnp::word));
            publisher.put(std::move(words));
        });
        expect(n == 0, "a malformed message is dropped, and nothing escaped");
    }

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
