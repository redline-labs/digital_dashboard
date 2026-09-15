// SPDX-License-Identifier: GPL-3.0-or-later
//
// HealthSnapshot <-> NodeHealth. A monitor reads samples from every node on the
// bus, including newer builds and buggy ones, so what decode() does with an
// enumerant it has never heard of, or a message far larger than any real node
// sends, is pinned here.
#include "node_health/codec.h"

#include "check.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <string>
#include <vector>

using node_health::CheckReport;
using node_health::HealthSnapshot;
using node_health::State;

namespace
{

HealthSnapshot sample()
{
    HealthSnapshot snapshot;
    snapshot.node = "megasquirt";
    snapshot.zid = "0123456789abcdef";
    snapshot.state = State::degraded;
    snapshot.sequence = 42;
    snapshot.uptime_ms = 12'345;
    snapshot.period_ms = 1000;
    snapshot.pid = 777;
    snapshot.checks = {
        CheckReport{"can_rx", State::degraded, "nothing for 2.0 s", 2000},
        CheckReport{"publishers", State::ok, "", 12'000},
    };
    return snapshot;
}

std::vector<std::uint8_t> bytesOf(capnp::MallocMessageBuilder& message)
{
    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const auto bytes = words.asBytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

void testRoundTrip()
{
    capnp::MallocMessageBuilder message;
    node_health::encode(message.initRoot<::NodeHealth>(), sample());
    const auto decoded = node_health::decodePayload(bytesOf(message));

    test::check(decoded.has_value(), "a sample decodes");
    if (!decoded)
    {
        return;
    }
    const HealthSnapshot expected = sample();
    test::check(decoded->node == expected.node && decoded->zid == expected.zid, "names survive");
    test::check(decoded->state == State::degraded, "state survives");
    test::check(decoded->sequence == 42 && decoded->uptime_ms == 12'345 && decoded->period_ms == 1000 &&
                    decoded->pid == 777,
                "numbers survive");
    test::check(decoded->checks.size() == 2, "both checks survive");
    if (decoded->checks.size() == 2)
    {
        test::check(decoded->checks[0].name == "can_rx" && decoded->checks[0].state == State::degraded &&
                        decoded->checks[0].detail == "nothing for 2.0 s" &&
                        decoded->checks[0].state_age_ms == 2000,
                    "a check survives field by field, in order");
    }
}

void testEveryStateRoundTrips()
{
    for (State state : {State::unknown, State::starting, State::ok, State::degraded, State::fault,
                        State::stopping})
    {
        capnp::MallocMessageBuilder message;
        HealthSnapshot snapshot;
        snapshot.state = state;
        node_health::encode(message.initRoot<::NodeHealth>(), snapshot);
        test::check(node_health::decode(message.getRoot<::NodeHealth>().asReader()).state == state,
                    "state " + std::string(node_health::to_string(state)) + " round trips");
    }
}

void testUnknownEnumerantReadsAsUnknown()
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<::NodeHealth>();
    root.setState(static_cast<::HealthState>(42));
    auto checks = root.initChecks(1);
    checks[0].setState(static_cast<::HealthState>(9));
    const HealthSnapshot decoded = node_health::decode(root.asReader());
    test::check(decoded.state == State::unknown, "a newer node's overall state reads as unknown");
    test::check(decoded.checks.size() == 1 && decoded.checks[0].state == State::unknown,
                "and so does a check's");
}

void testOversizedIsBounded()
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<::NodeHealth>();
    auto checks = root.initChecks(static_cast<unsigned>(node_health::kMaxChecks + 10));
    // 300 bytes of two-byte characters: the cut at 256 lands mid-character
    // unless decode backs up to a boundary.
    std::string long_detail;
    for (int i = 0; i < 150; ++i)
    {
        long_detail += "\xc3\xa9";
    }
    checks[0].setDetail(long_detail);
    root.setNode(std::string(1000, 'n'));

    const HealthSnapshot decoded = node_health::decode(root.asReader());
    test::check(decoded.checks.size() == node_health::kMaxChecks, "the check list is capped");
    test::check(decoded.node.size() == node_health::kMaxTextBytes, "long text is capped");
    const std::string& detail = decoded.checks.empty() ? std::string() : decoded.checks[0].detail;
    test::check(detail.size() <= node_health::kMaxTextBytes && detail.size() % 2 == 0,
                "a multi-byte character is never cut in half");
}

void testEmptyAndGarbage()
{
    capnp::MallocMessageBuilder message;
    message.initRoot<::NodeHealth>();
    const auto empty = node_health::decodePayload(bytesOf(message));
    test::check(empty.has_value() && empty->checks.empty() && empty->state == State::unknown,
                "an empty message decodes to an empty, unknown report");

    test::check(!node_health::decodePayload({}).has_value(), "no bytes is not a sample");
    test::check(!node_health::decodePayload({1, 2, 3}).has_value(), "a ragged payload is not a sample");
    test::check(!node_health::decodePayload(std::vector<std::uint8_t>(64, 0xff)).has_value(),
                "word-aligned garbage is not a sample");
}

}  // namespace

int main()
{
    testRoundTrip();
    testEveryStateRoundTrips();
    testUnknownEnumerantReadsAsUnknown();
    testOversizedIsBounded();
    testEmptyAndGarbage();
    return test::finish();
}
