// Behaviour of the generated parser that the differential test cannot see.
//
// test_golden replays payloads and compares numbers, so it says nothing about
// frame length checks, handler registration or when a multiplexed message is
// considered complete. Each of those was a real defect:
//
//   - a short frame was zero padded by the caller and decoded as though the
//     padding were readings, because nothing carried the length;
//   - registering a handler assigned to a single std::function, so an
//     aggregator silently discarded a directly registered handler for the same
//     message;
//   - a multiplexed message only ever reported a complete batch, so a device
//     that sends some groups conditionally went permanently silent.

#include "dbc_test_features_parser.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace dbc_test_features;

namespace
{

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures += 1;
    }
}

// A frame for the multiplexed message, in multiplex group `group`.
std::array<uint8_t, 8> statusFrame(uint8_t group)
{
    Multiplexed_t message;
    message.MuxIndex = group;
    return message.encode();
}

void testShortFrameRejected()
{
    dbc_test_features_t db;

    const std::array<uint8_t, 8> full = statusFrame(0);

    // Exactly enough is fine.
    check(db.decode(Multiplexed_t::id, full) !=
              dbc_test_features_t::Messages::Unknown,
          "a full length frame should decode");

    // One byte short is not, and must not be reported as a decoded message.
    const std::span<const uint8_t> truncated(full.data(), full.size() - 1);
    check(db.decode(Multiplexed_t::id, truncated) ==
              dbc_test_features_t::Messages::Unknown,
          "a frame shorter than the message must not decode");

    // An empty frame is the degenerate case of the same thing.
    check(db.decode(Multiplexed_t::id, std::span<const uint8_t>{}) ==
              dbc_test_features_t::Messages::Unknown,
          "an empty frame must not decode");

    // An id that is not ours stays unknown however long the frame is.
    check(db.decode(0x7FFu, full) == dbc_test_features_t::Messages::Unknown,
          "an unknown id must not decode");
}

void testShortFrameLeavesValuesAlone()
{
    dbc_test_features_t db;

    std::array<uint8_t, 8> frame = statusFrame(0);
    frame[7] = 0xAB;
    (void)db.decode(Multiplexed_t::id, frame);

    // Whatever the last byte decoded to, a rejected short frame must not
    // change it -- the old behaviour overwrote it with the caller's padding.
    std::vector<uint8_t> shortFrame(frame.begin(), frame.end() - 1);
    const auto before = db.Multiplexed;
    // Deliberately ignored: the point is what it did *not* write.
    (void)db.decode(Multiplexed_t::id, shortFrame);

    bool identical = true;
    db.Multiplexed.visit([&](const auto &value, auto tag) {
        using Sig = decltype(tag);
        before.visit([&](const auto &original, auto originalTag) {
            using OriginalSig = decltype(originalTag);
            if (Sig::name == OriginalSig::name)
            {
                if (static_cast<int64_t>(value) != static_cast<int64_t>(original))
                {
                    identical = false;
                }
            }
        });
    });

    check(identical, "a rejected short frame must not modify already decoded values");
}

void testHandlersAccumulate()
{
    dbc_test_features_parser parser;

    int first = 0;
    int second = 0;
    parser.on_Multiplexed([&](const Multiplexed_t &) { first += 1; });
    parser.on_Multiplexed([&](const Multiplexed_t &) { second += 1; });

    // A whole batch, so the complete-batch handlers fire exactly once.
    for (uint8_t group : Multiplexed_t::multiplexor_group_indexes)
    {
        const auto frame = statusFrame(group);
        parser.handle_can_frame(Multiplexed_t::id, frame);
    }

    check(first == 1, "the first registered handler should have fired");
    check(second == 1, "the second registered handler must not have been discarded");
}

void testMultiplexGatingPolicies()
{
    dbc_test_features_parser parser;

    int complete = 0;
    int perFrame = 0;
    parser.on_Multiplexed([&](const Multiplexed_t &) { complete += 1; });
    parser.on_Multiplexed_each_frame(
        [&](const Multiplexed_t &) { perFrame += 1; });

    const auto groups = Multiplexed_t::multiplexor_group_indexes;

    // Only the first group, repeatedly: the batch never completes.
    for (int i = 0; i < 3; ++i)
    {
        const auto frame = statusFrame(static_cast<uint8_t>(groups[0]));
        parser.handle_can_frame(Multiplexed_t::id, frame);
    }

    check(complete == 0, "an incomplete batch must not fire the complete-batch handler");
    check(perFrame == 3, "every frame should reach the each-frame handler");

    // Now finish the batch.
    for (size_t i = 1; i < groups.size(); ++i)
    {
        const auto frame = statusFrame(static_cast<uint8_t>(groups[i]));
        parser.handle_can_frame(Multiplexed_t::id, frame);
    }

    check(complete == 1, "a complete batch should fire the complete-batch handler once");
    check(perFrame == 3 + static_cast<int>(groups.size() - 1),
          "the each-frame handler should have seen every frame");
}

void testEnumeratorNames()
{
    // These names existing at all is the assertion, and it is a compile-time
    // one: the generator used to append the raw value to every enumerator
    // whether or not anything collided, so these read Off_0 and On_1.
    using Plain = WithValues_t::sig_Plain_t::Values;
    static_assert(static_cast<int64_t>(Plain::Off) == 0);
    static_assert(static_cast<int64_t>(Plain::On) == 1);
    static_assert(static_cast<int64_t>(Plain::Standby) == 2);
}

// Frames for two of the plain, non-multiplexed messages.
std::array<uint8_t, 8> valuesFrame()
{
    WithValues_t message;
    return message.encode();
}

std::array<uint8_t, 8> floatFrame()
{
    FloatSignals_t message;
    return message.encode();
}

void testAggregatorWaitsForEveryMember()
{
    dbc_test_features_parser parser;

    int completed = 0;
    parser.add_message_aggregator<dbc_test_features_t::Messages::WithValues,
                                  dbc_test_features_t::Messages::FloatSignals>(
        [&](const dbc_test_features_t &) { completed += 1; });

    // The first member alone, repeatedly: never a complete set.
    for (int i = 0; i < 5; ++i)
    {
        parser.handle_can_frame(WithValues_t::id, valuesFrame());
    }
    check(completed == 0, "an aggregator must not fire before every member has arrived");

    parser.handle_can_frame(FloatSignals_t::id, floatFrame());
    check(completed == 1, "an aggregator should fire once the set is complete");

    // And it resets, rather than firing on every subsequent frame.
    parser.handle_can_frame(FloatSignals_t::id, floatFrame());
    check(completed == 1, "an aggregator must reset after firing");

    parser.handle_can_frame(WithValues_t::id, valuesFrame());
    parser.handle_can_frame(FloatSignals_t::id, floatFrame());
    check(completed == 2, "an aggregator should fire again on the next complete set");
}

void testAggregatorAlignsToTheFirstMember()
{
    dbc_test_features_parser parser;

    int completed = 0;
    parser.add_message_aggregator<dbc_test_features_t::Messages::WithValues,
                                  dbc_test_features_t::Messages::FloatSignals>(
        [&](const dbc_test_features_t &) { completed += 1; });

    // Second member first. It must not count towards a set, or a batch could
    // be assembled from halves of two different cycles.
    parser.handle_can_frame(FloatSignals_t::id, floatFrame());
    check(completed == 0, "a trailing member seen before the first must not count");

    parser.handle_can_frame(WithValues_t::id, valuesFrame());
    check(completed == 0, "the first member alone is still not a complete set");

    parser.handle_can_frame(FloatSignals_t::id, floatFrame());
    check(completed == 1, "the set completes once the members arrive in order");
}

void testAggregatorCoexistsWithDirectHandlers()
{
    dbc_test_features_parser parser;

    // The bug this pins: registering used to assign to a single std::function,
    // so the aggregator's registration silently threw this handler away.
    int direct = 0;
    parser.on_WithValues([&](const WithValues_t &) { direct += 1; });

    int firstAggregate = 0;
    int secondAggregate = 0;
    parser.add_message_aggregator<dbc_test_features_t::Messages::WithValues,
                                  dbc_test_features_t::Messages::FloatSignals>(
        [&](const dbc_test_features_t &) { firstAggregate += 1; });
    // A second aggregator sharing a message with the first.
    parser.add_message_aggregator<dbc_test_features_t::Messages::WithValues,
                                  dbc_test_features_t::Messages::DoubleSignal>(
        [&](const dbc_test_features_t &) { secondAggregate += 1; });

    parser.handle_can_frame(WithValues_t::id, valuesFrame());
    parser.handle_can_frame(FloatSignals_t::id, floatFrame());
    parser.handle_can_frame(DoubleSignal_t::id, DoubleSignal_t{}.encode());

    check(direct == 1, "a direct handler must survive an aggregator registering after it");
    check(firstAggregate == 1, "the first aggregator should have completed");
    check(secondAggregate == 1, "a second aggregator sharing a message should also complete");
}

void testAggregatorSeesDecodedValues()
{
    dbc_test_features_parser parser;

    // The aggregate callback is handed the database, so the point is that the
    // members hold what was just decoded rather than a stale or empty struct.
    bool matched = false;
    parser.add_message_aggregator<dbc_test_features_t::Messages::WithValues,
                                  dbc_test_features_t::Messages::FloatSignals>(
        [&](const dbc_test_features_t &db) {
            matched = (static_cast<int64_t>(db.WithValues.Plain) == 7);
        });

    WithValues_t source;
    source.Plain = static_cast<WithValues_t::sig_Plain_t::Type>(7);
    parser.handle_can_frame(WithValues_t::id, source.encode());
    parser.handle_can_frame(FloatSignals_t::id, floatFrame());

    check(matched, "the aggregate callback should see freshly decoded values");
}

void testRoundTrip()
{
    for (uint32_t group : Multiplexed_t::multiplexor_group_indexes)
    {
        Multiplexed_t message;
        message.MuxIndex = group;
        const auto encoded = message.encode();

        Multiplexed_t decoded;
        check(decoded.decode(encoded), "a self-encoded frame should decode");
        check(static_cast<uint64_t>(decoded.MuxIndex) == group,
              "the multiplexor should survive a round trip");
    }
}

} // namespace

int main()
{
    testShortFrameRejected();
    testShortFrameLeavesValuesAlone();
    testHandlersAccumulate();
    testMultiplexGatingPolicies();
    testEnumeratorNames();
    testAggregatorWaitsForEveryMember();
    testAggregatorAlignsToTheFirstMember();
    testAggregatorCoexistsWithDirectHandlers();
    testAggregatorSeesDecodedValues();
    testRoundTrip();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("generated api: all checks passed\n");
    return 0;
}
