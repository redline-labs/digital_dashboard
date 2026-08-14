// SPDX-License-Identifier: GPL-3.0-or-later
//
// ZenohAsyncClient: the non-blocking counterpart to ZenohClient.
//
// What is worth pinning here is not "a reply arrives" -- that is the easy half.
// It is that the callback fires EXACTLY ONCE in every outcome, including the
// ones nothing on the wire announces:
//
//   * nobody serving the key. There is no "no such service" message in zenoh;
//     the query simply completes. Without the drop handler turning that into a
//     callback, a caller waits forever for something that was never coming --
//     and a map widget with one tile request stuck like that leaks a request
//     slot per tile.
//
//   * two responders. The first answer stands and the second must not
//     re-enter the caller's callback with a second, different tile.
//
//   * a truncated payload. capnp reads a short buffer as a message whose every
//     field is default, so "malformed" and "a valid response full of zeros"
//     are the same bytes unless the length is checked first.
//
// The client and the service share this process's zenoh session, which is how
// zenoh routes a local query to a local queryable. That makes this a `net` test:
// it opens a session.

#include "pub_sub/zenoh_async_client.h"
#include "pub_sub/zenoh_service.h"

#include "can_bridge.capnp.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using Client = pub_sub::ZenohAsyncClient<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse>;
using Service = pub_sub::ZenohService<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse>;

// Collects what the callback was handed, from whichever zenoh thread ran it.
struct Outcome
{
    std::mutex mutex;
    std::condition_variable cv;
    int calls { 0 };
    Client::Status status { Client::Status::Failed };
    std::uint32_t nominalBps { 0 };
    bool sawResponsePointer { false };

    void record(Client::Status s, const CanBridgeSetBitrateResponse::Reader* response)
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);
            ++calls;
            status = s;
            sawResponsePointer = (response != nullptr);
            if (response != nullptr)
            {
                nominalBps = response->getActualNominalBps();
            }
        }
        cv.notify_all();
    }

    // Wait for the first call, then keep waiting a little longer to catch a
    // SECOND one. A test that returned the moment the callback fired could not
    // tell "once" from "twice".
    bool awaitOne(std::chrono::milliseconds settle = std::chrono::milliseconds(400))
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool arrived =
            cv.wait_for(lock, std::chrono::seconds(5), [this]() { return calls > 0; });
        lock.unlock();
        std::this_thread::sleep_for(settle);
        return arrived;
    }
};

constexpr const char* kServedKey = "test/async_client/served";
constexpr const char* kUnservedKey = "test/async_client/nobody_home";
constexpr const char* kSlowKey = "test/async_client/slow";

// ============================================================================

void test_a_reply_is_delivered_once()
{
    Service service(kServedKey, [](const CanBridgeSetBitrateRequest::Reader& request,
                                   CanBridgeSetBitrateResponse::Builder& response) {
        response.setOk(true);
        // Echo the request back, so the test can tell a real round trip from a
        // default-constructed response that happens to look plausible.
        response.setActualNominalBps(request.getNominalBps());
    });

    Client client(kServedKey, 2000);

    Outcome outcome;
    const bool sent = client.request(
        [](CanBridgeSetBitrateRequest::Builder& request) {
            request.setChannel("can0");
            request.setNominalBps(500000);
        },
        [&outcome](Client::Status status, const CanBridgeSetBitrateResponse::Reader* response) {
            outcome.record(status, response);
        });

    check(sent, "the request was sent");
    check(outcome.awaitOne(), "a reply arrived");
    check(outcome.calls == 1, "the callback fired exactly once");
    check(outcome.status == Client::Status::Ok, "the status is Ok");
    check(outcome.sawResponsePointer, "Ok comes with a non-null response");
    check(outcome.nominalBps == 500000, "the response is the service's, not a default-constructed one");
}

void test_no_responder_still_calls_back()
{
    // No service on this key. Nothing on the wire says so -- the query just
    // completes -- and the drop handler is the only thing that turns that into
    // an answer.
    Client client(kUnservedKey, 300);

    Outcome outcome;
    const bool sent = client.request(
        [](CanBridgeSetBitrateRequest::Builder& request) { request.setChannel("nowhere"); },
        [&outcome](Client::Status status, const CanBridgeSetBitrateResponse::Reader* response) {
            outcome.record(status, response);
        });

    check(sent, "a request to an unserved key is still sent");
    check(outcome.awaitOne(), "an unserved key still calls back");
    check(outcome.calls == 1, "it calls back exactly once");
    check(outcome.status == Client::Status::NoReply, "the status is NoReply");
    check(!outcome.sawResponsePointer, "NoReply comes with a null response");
}

void test_two_responders_deliver_one_answer()
{
    // Two services on one key is a configuration problem, not something the
    // client resolves -- but it must not turn into two callbacks, because a
    // caller that started one request and finished two has state it did not
    // account for.
    Service first("test/async_client/twice", [](const CanBridgeSetBitrateRequest::Reader&,
                                                CanBridgeSetBitrateResponse::Builder& response) {
        response.setOk(true);
        response.setActualNominalBps(111);
    });
    Service second("test/async_client/twice", [](const CanBridgeSetBitrateRequest::Reader&,
                                                 CanBridgeSetBitrateResponse::Builder& response) {
        response.setOk(true);
        response.setActualNominalBps(222);
    });

    Client client("test/async_client/twice", 1000);

    Outcome outcome;
    client.request(
        [](CanBridgeSetBitrateRequest::Builder& request) { request.setChannel("can0"); },
        [&outcome](Client::Status status, const CanBridgeSetBitrateResponse::Reader* response) {
            outcome.record(status, response);
        });

    check(outcome.awaitOne(std::chrono::milliseconds(1200)), "a reply arrived");
    check(outcome.calls == 1, "two responders still produce exactly one callback");
}

void test_overlapping_requests_do_not_share_state()
{
    // Each request owns its own builder. If they shared one, the second call
    // would overwrite the first's payload before it was serialised and both
    // would ask the same question -- which looks like a working client right up
    // until two tiles come back identical.
    Service service("test/async_client/echo", [](const CanBridgeSetBitrateRequest::Reader& request,
                                                 CanBridgeSetBitrateResponse::Builder& response) {
        response.setOk(true);
        response.setActualNominalBps(request.getNominalBps());
    });

    Client client("test/async_client/echo", 2000);

    constexpr int kCount = 16;
    std::vector<std::unique_ptr<Outcome>> outcomes;
    outcomes.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        outcomes.push_back(std::make_unique<Outcome>());
    }

    for (int i = 0; i < kCount; ++i)
    {
        Outcome* outcome = outcomes[static_cast<std::size_t>(i)].get();
        client.request(
            [i](CanBridgeSetBitrateRequest::Builder& request) {
                request.setChannel("can0");
                request.setNominalBps(static_cast<std::uint32_t>(1000 + i));
            },
            [outcome](Client::Status status,
                      const CanBridgeSetBitrateResponse::Reader* response) {
                outcome->record(status, response);
            });
    }

    int wrong = 0;
    int notOnce = 0;
    for (int i = 0; i < kCount; ++i)
    {
        Outcome& outcome = *outcomes[static_cast<std::size_t>(i)];
        outcome.awaitOne(std::chrono::milliseconds(0));
        const std::lock_guard<std::mutex> guard(outcome.mutex);
        if (outcome.calls != 1)
        {
            ++notOnce;
        }
        if (outcome.status != Client::Status::Ok ||
            outcome.nominalBps != static_cast<std::uint32_t>(1000 + i))
        {
            ++wrong;
        }
    }

    check(notOnce == 0, "every overlapping request called back exactly once");
    check(wrong == 0, "every overlapping request got its own answer");
}

void test_destroying_the_client_mid_flight_is_safe()
{
    // The map widget destroys its file source when a layout changes, with tile
    // requests outstanding. The per-request state is owned by the zenoh
    // closures rather than by the client, precisely so this is not a use after
    // free.
    Service service(kSlowKey, [](const CanBridgeSetBitrateRequest::Reader&,
                                 CanBridgeSetBitrateResponse::Builder& response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        response.setOk(true);
        response.setActualNominalBps(9999);
    });

    auto outcome = std::make_shared<Outcome>();

    {
        Client client(kSlowKey, 2000);
        client.request(
            [](CanBridgeSetBitrateRequest::Builder& request) { request.setChannel("can0"); },
            [outcome](Client::Status status,
                      const CanBridgeSetBitrateResponse::Reader* response) {
                outcome->record(status, response);
            });
        // client goes out of scope here, well before the service answers.
    }

    check(outcome->awaitOne(), "the callback still fires after the client is gone");
    check(outcome->calls == 1, "and fires exactly once");
    check(outcome->status == Client::Status::Ok, "and carries the real answer");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_ERROR("No zenoh session available; cannot run.");
        return 1;
    }

    test_a_reply_is_delivered_once();
    test_no_responder_still_calls_back();
    test_two_responders_deliver_one_answer();
    test_overlapping_requests_do_not_share_state();
    test_destroying_the_client_mid_flight_is_safe();

    pub_sub::SessionManager::shutdown();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all async client checks passed");
    return 0;
}
