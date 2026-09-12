// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XBus message frame, and the stream framer that finds one in a byte
// stream.
//
// Three things here are worth more than the rest.
//
// The first is that the parser is exercised at COMPILE time as well as at run
// time. Every static_assert below stops the build if it stops being true,
// which is the property this library is built around -- see byte_order.h.
//
// The second is the four VENDOR vectors. No MTi has been on the bench, so
// almost everything in this library is checked against itself. These four
// messages were printed complete, with their checksums, by Xsens in the LLCP,
// and they are the only bytes here that came out of the vendor's encoder. If
// the checksum rule or the header layout were wrong, these would fail.
//
// The third is resynchronisation. XBus has no escaping and no trailer, so 0xFA
// is an ordinary payload byte. The interesting cases are therefore not the
// well-formed ones: they are the corrupted, the truncated, the fragmented, and
// above all the message with a preamble sitting inside its payload.

#include "xbus/framer.h"
#include "xbus/message.h"

#include "golden/golden_messages.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <string>
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

using namespace xbus;

template <std::size_t N>
constexpr std::span<const std::uint8_t> bytes(const std::array<std::uint8_t, N>& a)
{
    return std::span<const std::uint8_t>(a.data(), a.size());
}

// ============================================================================
// Compile-time: the vendor vectors
//
// These four are the load-bearing assertions in the whole library. Each one is
// a complete message Xsens printed, checksum included.
// ============================================================================

static_assert(parse_message(bytes(golden::kReqDeviceId)).has_value(),
              "the LLCP's own ReqDID bytes must parse");
static_assert(parse_message(bytes(golden::kReqBaudrate)).has_value(),
              "the LLCP's own ReqBaudrate bytes must parse");
static_assert(parse_message(bytes(golden::kSetBaudrateAck)).has_value(),
              "the LLCP's own SetBaudrateAck bytes must parse");
static_assert(parse_message(bytes(golden::kSetStringOutputTypeNone)).has_value(),
              "the LLCP's own SetStringOutputType bytes must parse");

constexpr MessageView kReqDid = *parse_message(bytes(golden::kReqDeviceId));
static_assert(kReqDid.bid == kBidMaster, "a host addresses the master BID");
static_assert(kReqDid.is(MessageId::ReqDeviceId));
static_assert(kReqDid.data.empty(), "ReqDID carries no payload");
static_assert(!kReqDid.extendedLength);

// The acknowledgement rule, against vendor bytes on both sides: 0x18 asks,
// 0x19 answers.
static_assert(ack_of(MessageId::ReqDeviceId) == static_cast<std::uint8_t>(MessageId::DeviceId));
static_assert(ack_of(0x18) == 0x19);
static_assert(parse_message(bytes(golden::kSetBaudrateAck))->id == ack_of(0x18),
              "SetBaudrateAck's id is SetBaudrate's plus one");

// The Req/Set distinction is a length, and nothing else. Both vendor vectors
// carry message id 0x18; this one is the request because its payload is empty.
static_assert(parse_message(bytes(golden::kReqBaudrate))->data.empty(),
              "a request is the zero-length form of its message id");

// And the naming that falls out of it. Getting this backwards in a log is how
// an afternoon disappears.
static_assert(describe_host_message(MessageId::OutputConfiguration, false) ==
                  std::string_view("req_output_configuration"));
static_assert(describe_host_message(MessageId::OutputConfiguration, true) ==
                  std::string_view("set_output_configuration"));
static_assert(describe_host_message(MessageId::GoToConfig, false) ==
                  std::string_view("goto_config"));

// ============================================================================
// Compile-time: round-tripping our encoder against the vendor's bytes
//
// make_message() is what every command in commands.h is built from. Requiring
// it to reproduce a vendor message byte for byte is the strongest statement
// available about the encoder without a device to refuse it.
// ============================================================================

static_assert(make_message(MessageId::ReqDeviceId, std::array<std::uint8_t, 0> {}) ==
                  golden::kReqDeviceId,
              "encoding ReqDID must reproduce the LLCP's bytes exactly");

static_assert(make_message(MessageId::OptionFlags, std::array<std::uint8_t, 0> {})[4] != 0,
              "a checksum of zero here would mean the sum was never taken");

// The two-byte-payload vendor vector, rebuilt. MID 0x8E is SetStringOutputType,
// which is not in XBUS_MESSAGE_TABLE -- this library never sends it -- so it
// goes through encode_message() with a cast rather than through the enum.
constexpr std::array<std::uint8_t, 7> encodeStringOutputNone()
{
    std::array<std::uint8_t, 7> out {};
    constexpr std::array<std::uint8_t, 2> payload { 0x00, 0x00 };
    const Result<std::size_t> used =
        encode_message(static_cast<MessageId>(0x8E), bytes(payload), out);
    return used.has_value() ? out : std::array<std::uint8_t, 7> {};
}

static_assert(encodeStringOutputNone() == golden::kSetStringOutputTypeNone,
              "encoding a two-byte payload must reproduce the LLCP's bytes exactly");

// ============================================================================
// Compile-time: malformed input
// ============================================================================

// Not a preamble.
static_assert(parse_message(std::array<std::uint8_t, 5> { 0x00, 0xFF, 0x30, 0x00, 0xD1 })
                  .error()
                  .kind == ErrorKind::BadFraming);

// Header present, payload not.
static_assert(parse_message(std::array<std::uint8_t, 4> { 0xFA, 0xFF, 0x36, 0x40 })
                  .error()
                  .kind == ErrorKind::Truncated);

// Nothing at all.
static_assert(parse_message(std::span<const std::uint8_t> {}).error().kind == ErrorKind::Truncated);

// One bit flipped in the checksum.
static_assert(parse_message(std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x30, 0x00, 0xD0 })
                  .error()
                  .kind == ErrorKind::BadChecksum);

// A length of 0xFF promises two extended-length bytes that are not there.
static_assert(parse_message(std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x36, 0xFF, 0x00 })
                  .error()
                  .kind == ErrorKind::Truncated);

// An extended length beyond what the protocol carries.
static_assert(parse_message(std::array<std::uint8_t, 7> { 0xFA, 0xFF, 0x36, 0xFF, 0xFF, 0xFF, 0x00 })
                  .error()
                  .kind == ErrorKind::TooLong);

// An extended message id. This library refuses rather than guesses -- see
// parse_message(). The assertion exists so that a future change making this
// decodable is a deliberate act rather than an accident.
static_assert(parse_message(std::array<std::uint8_t, 6> { 0xFA, 0xFF, 0xEE, 0x00, 0x00, 0x13 })
                  .error()
                  .kind == ErrorKind::UnsupportedFormat,
              "an extended message id must be refused, not half-decoded");

// ============================================================================
// Run time: the framer
// ============================================================================

std::vector<std::uint8_t> concat(std::initializer_list<std::span<const std::uint8_t>> parts)
{
    std::vector<std::uint8_t> out;
    for (const std::span<const std::uint8_t>& part : parts)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

void testFramerFindsBackToBackMessages()
{
    Framer framer;
    framer.push(concat({ bytes(golden::kGoToConfig),
                         bytes(golden::kMtData2PacketCounter),
                         bytes(golden::kReqDeviceId) }));

    std::vector<std::uint8_t> ids;
    while (const std::optional<MessageView> message = framer.next())
    {
        ids.push_back(message->id);
    }

    check(ids.size() == 3, "three back-to-back messages yield three");
    check(ids == std::vector<std::uint8_t> { 0x30, 0x36, 0x00 }, "and in order");
    check(framer.stats().resyncs == 0, "a clean stream needs no resync");
    check(framer.buffered() == 0, "and leaves nothing buffered");
}

void testPreambleInsidePayloadDoesNotMisframe()
{
    // The case that decides whether the framer is correct. Resynchronising by
    // scanning for the next 0xFA would find the one at offset 7, inside the
    // status word, and frame garbage from there.
    Framer framer;
    framer.push(bytes(golden::kMtData2WithPreambleInPayload));

    const std::optional<MessageView> message = framer.next();
    check(message.has_value(), "a message whose payload contains 0xFA still parses");

    if (message)
    {
        check(message->data.size() == 7, "and keeps its whole payload");
        check(message->data[3] == 0xFA && message->data[5] == 0xFA,
              "including the preamble bytes inside it");
    }

    check(!framer.next().has_value(), "and there is nothing behind it");
    check(framer.stats().messages == 1, "exactly one message was framed");
}

void testFragmentedDelivery()
{
    // What a serial port actually does: hand over the message one byte at a
    // time. Nothing may be emitted until the last byte arrives.
    const std::vector<std::uint8_t> whole = concat({ bytes(golden::kMtData2PacketCounter) });

    Framer framer;
    for (std::size_t i = 0; i + 1 < whole.size(); ++i)
    {
        framer.push(std::span<const std::uint8_t>(&whole[i], 1));
        check(!framer.next().has_value(), "no message before its last byte arrives");
    }

    framer.push(std::span<const std::uint8_t>(&whole[whole.size() - 1], 1));
    check(framer.next().has_value(), "and one as soon as it does");
}

void testResyncAfterGarbage()
{
    // Bytes that cannot start a message: one resync each, no ambiguity.
    const std::array<std::uint8_t, 3> garbage { 0x11, 0x22, 0x33 };

    Framer framer;
    framer.push(concat({ bytes(garbage), bytes(golden::kGoToConfig) }));

    const std::optional<MessageView> message = framer.next();
    check(message.has_value(), "a message behind garbage is still found");
    check(message && message->is(MessageId::GoToConfig), "and is the right one");
    check(framer.stats().resyncs == 3, "one resync per discarded byte");
    check(framer.stats().droppedBytes == 3, "and three bytes discarded");
}

void testFalsePreambleDefersRatherThanMisframes()
{
    // A property of XBus worth stating outright, because it looks like a bug
    // the first time it is seen in a log.
    //
    // There is no trailer and no escaping, so a 0xFA in garbage -- or in the
    // payload of a message whose header was lost -- is indistinguishable from
    // a real preamble until its declared length and checksum have been
    // checked. When the byte after it claims a long payload, the framer CANNOT
    // decide yet, and correctly reports "not enough bytes" rather than
    // guessing. A stream stalled behind a false preamble is not wedged: the
    // candidate fails its checksum as soon as enough bytes arrive, and
    // everything behind it comes out at once.
    //
    // The alternative -- scanning ahead to the next preamble on a failed
    // candidate -- would decide immediately and be wrong, because a real
    // message's payload contains 0xFA several times a second.
    // LEN is the fourth byte, not the second: a false preamble only defers the
    // decision once its whole header is present and plausible.
    const std::array<std::uint8_t, 4> falseStart { 0xFA, 0x00, 0x00, 0x64 };  // claims 100 bytes

    Framer framer;
    framer.push(concat({ bytes(falseStart), bytes(golden::kGoToConfig) }));

    check(!framer.next().has_value(),
          "a false preamble claiming a long payload defers the decision");
    check(framer.stats().messages == 0, "and emits nothing while it is undecided");

    // Enough further bytes for the candidate to be judged and rejected.
    const std::vector<std::uint8_t> filler(128, 0x00);
    framer.push(filler);

    std::vector<std::uint8_t> ids;
    while (const std::optional<MessageView> message = framer.next())
    {
        ids.push_back(message->id);
    }

    check(ids.size() == 1 && ids[0] == static_cast<std::uint8_t>(MessageId::GoToConfig),
          "and the real message behind it arrives once the candidate is disproved");
    check(framer.stats().resyncs >= 1, "having cost at least one resync");
}

void testCorruptedMessageIsSkippedNotWedged()
{
    // The failure mode this class exists to prevent: one bad byte must cost at
    // most one message, never the stream.
    std::vector<std::uint8_t> corrupt = concat({ bytes(golden::kMtData2PacketCounter) });
    corrupt[6] ^= 0x01;

    Framer framer;
    framer.push(concat({ std::span<const std::uint8_t>(corrupt), bytes(golden::kGoToConfig) }));

    std::vector<std::uint8_t> ids;
    while (const std::optional<MessageView> message = framer.next())
    {
        ids.push_back(message->id);
    }

    check(framer.stats().checksumErrors >= 1, "the corrupted message is counted");
    check(ids.size() == 1 && ids[0] == 0x30, "and the one behind it still arrives");
}

void testOverflowClearsRatherThanGrows()
{
    // The cap is raised to kMaxMessageSize in the constructor whatever the
    // caller asks for, because a framer that could not hold one maximum-size
    // message would reject legal traffic. So the junk has to exceed that, not
    // the requested 1024.
    Framer framer(1024);

    const std::vector<std::uint8_t> junk(kMaxMessageSize * 2, 0xFA);
    framer.push(junk);

    check(framer.stats().overflows == 1, "a stream that is not XBus trips the cap");
    check(framer.buffered() == 0, "and the buffer is cleared rather than grown");

    // And the framer still works afterwards.
    framer.push(bytes(golden::kGoToConfig));
    check(framer.next().has_value(), "the framer recovers after an overflow");
}

void testResetDiscardsPartialMessageButKeepsStats()
{
    Framer framer;

    framer.push(bytes(golden::kGoToConfig));
    check(framer.next().has_value(), "priming the stats with one good message");
    const std::uint64_t messages = framer.stats().messages;

    // Only the header of the next message. Its declared length means anything
    // pushed behind it is part of that message until proven otherwise, which
    // is why the priming above happens first.
    framer.push(std::span<const std::uint8_t>(golden::kMtData2PacketCounter.data(), 4));
    check(!framer.next().has_value(), "a partial message emits nothing");
    check(framer.buffered() == 4, "and is held");

    framer.reset();
    check(framer.buffered() == 0, "reset discards the buffer");
    check(framer.stats().messages == messages,
          "but not the stats: they describe the link, not the connection");
}

void testViewStaysValidUntilNextPush()
{
    // The contract next() advertises: a caller may drain a whole read into a
    // container of views before touching any of them.
    Framer framer;
    framer.push(concat({ bytes(golden::kMtData2PacketCounter),
                         bytes(golden::kMtData2WithPreambleInPayload) }));

    std::vector<MessageView> drained;
    while (const std::optional<MessageView> message = framer.next())
    {
        drained.push_back(*message);
    }

    check(drained.size() == 2, "both messages drained");
    if (drained.size() == 2)
    {
        check(drained[0].data.size() == 5, "the first view still reads correctly");
        check(drained[1].data.size() == 7, "and so does the second");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testFramerFindsBackToBackMessages();
    testPreambleInsidePayloadDoesNotMisframe();
    testFragmentedDelivery();
    testResyncAfterGarbage();
    testFalsePreambleDefersRatherThanMisframes();
    testCorruptedMessageIsSkippedNotWedged();
    testOverflowClearsRatherThanGrows();
    testResetDiscardsPartialMessageButKeepsStats();
    testViewStaysValidUntilNextPush();

    return failures == 0 ? 0 : 1;
}
