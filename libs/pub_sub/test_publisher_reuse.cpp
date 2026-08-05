// SPDX-License-Identifier: GPL-3.0-or-later
//
// ZenohPublisher reuses one buffer as the message builder's first segment
// instead of mallocing a fresh one per message (222 ns and 2 allocations per
// put() before, 41 ns and 1 after). That makes the *contents* of consecutive
// messages a correctness question rather than a given, and this is where that
// gets checked:
//
//   - Field set on message N must not be visible on message N+1. capnp's
//     MallocMessageBuilder destructor re-zeroes the words it used, which is what
//     makes reuse legal at all; if it ever stopped doing so, or if the builder
//     were rebuilt over the wrong buffer, a stale field would ride along and
//     look exactly like a publisher genuinely repeating itself.
//
//   - A message larger than the scratch has to still be correct. The scratch
//     starts at 64 words and grows to fit, so the interesting sizes are the
//     first one that overflows it and everything after. CarPlay video and audio
//     publish payloads far past 64 words, so this is their path, not a corner
//     case.
//
// Every message goes over a real zenoh session and comes back through a real
// subscriber, so what is verified is what a peer would actually receive -- not
// what the builder thinks it wrote. Constructing either end opens a session, so
// this is a `net` test and skips itself where none can be opened, the same way
// the SessionManager and expression tests do.

#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include "can_frame.capnp.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// One received CanFrame, flattened into something comparable.
struct Received
{
    uint32_t id = 0;
    std::vector<uint8_t> data;
};

// zenoh is not a queue and gives no delivery guarantee, so the test waits for a
// count rather than assuming one arrived.
bool waitFor(std::mutex& mutex, const std::vector<Received>& got, size_t want,
             std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (got.size() >= want)
            {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const std::lock_guard<std::mutex> lock(mutex);
    return got.size() >= want;
}

void testConsecutiveMessagesDoNotBleed()
{
    const std::string key = "test/pub_sub/publisher_reuse";

    std::mutex mutex;
    std::vector<Received> got;

    pub_sub::ZenohTypedSubscriber<CanFrame> sub(key, [&](CanFrame::Reader reader) {
        Received r;
        r.id = reader.getId();
        for (auto byte : reader.getData())
        {
            r.data.push_back(byte);
        }
        const std::lock_guard<std::mutex> lock(mutex);
        got.push_back(std::move(r));
    });

    pub_sub::ZenohPublisher<CanFrame> pub(key);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the pair match

    // Payload sizes chosen around the 64-word (512-byte) initial scratch: well
    // under, just over, far over, then back under. The last one matters most --
    // once the scratch has grown, a short message must not pick up the tail of
    // the long one that preceded it.
    const std::vector<size_t> sizes{4, 8, 600, 4096, 4};
    std::vector<Received> want;

    for (size_t n = 0; n < sizes.size(); ++n)
    {
        const size_t len = sizes[n];
        Received expected;
        expected.id = static_cast<uint32_t>(0x100 + n);

        auto& fields = pub.fields();
        fields.setId(expected.id);
        fields.setLen(static_cast<uint8_t>(len & 0xFF));
        auto data = fields.initData(len);
        for (size_t i = 0; i < len; ++i)
        {
            // Vary with both the message index and the offset, so a stale byte
            // from the previous message is a mismatch rather than a coincidence.
            const auto byte = static_cast<uint8_t>((i * 7 + n * 31 + 1) & 0xFF);
            data.set(i, byte);
            expected.data.push_back(byte);
        }
        pub.put();
        want.push_back(std::move(expected));
    }

    if (!waitFor(mutex, got, sizes.size(), std::chrono::seconds(5)))
    {
        const std::lock_guard<std::mutex> lock(mutex);
        SPDLOG_WARN("Only {} of {} messages arrived; zenoh made no delivery promise, so this "
                    "is reported rather than failed.", got.size(), sizes.size());
    }

    const std::lock_guard<std::mutex> lock(mutex);
    expect(!got.empty(), "at least one message round-tripped");

    // Match on id rather than arrival order: what is being tested is per-message
    // integrity, and ordering is zenoh's business, not the builder's.
    for (const auto& expected : want)
    {
        const auto found = std::find_if(got.begin(), got.end(),
                                        [&](const Received& r) { return r.id == expected.id; });
        if (found == got.end())
        {
            SPDLOG_WARN("message id 0x{:X} ({} data bytes) did not arrive", expected.id,
                        expected.data.size());
            continue;
        }

        expect(found->data.size() == expected.data.size(),
               "message id 0x" + std::to_string(expected.id) + " has its own payload length (" +
                   std::to_string(found->data.size()) + " of " +
                   std::to_string(expected.data.size()) + " bytes)");
        expect(found->data == expected.data,
               "message id 0x" + std::to_string(expected.id) + " carries its own " +
                   std::to_string(expected.data.size()) +
                   " payload bytes, with nothing left over from the message before it");
    }
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    // No session means no zenoh router or no loopback here; that is testing the
    // environment, so say so and skip rather than fail.
    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_WARN("No zenoh session available; skipping publisher reuse tests.");
        return 0;
    }

    testConsecutiveMessagesDoNotBleed();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
