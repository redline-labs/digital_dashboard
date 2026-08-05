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
    db.decode(Multiplexed_t::id, frame);

    // Whatever the last byte decoded to, a rejected short frame must not
    // change it -- the old behaviour overwrote it with the caller's padding.
    std::vector<uint8_t> shortFrame(frame.begin(), frame.end() - 1);
    const auto before = db.Multiplexed;
    db.decode(Multiplexed_t::id, shortFrame);

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
    testRoundTrip();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("generated api: all checks passed\n");
    return 0;
}
