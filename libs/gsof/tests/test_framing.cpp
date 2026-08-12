// SPDX-License-Identifier: GPL-3.0-or-later
//
// The outer DCOL packet, and the stream framer that finds one in a byte
// stream.
//
// Two things here are worth more than the rest.
//
// The first is that the packet parser is exercised at COMPILE time as well as
// at run time. Every static_assert below is a claim that stops the build if it
// stops being true, which is the property the whole library is built around --
// see the header comment in byte_order.h.
//
// The second is resynchronisation. The reference ROS driver cannot recover
// from a corrupted byte: its page parser re-examines the same bytes forever
// and the GNSS feed silently stops. So the interesting cases here are not the
// well-formed ones, they are the corrupted, the truncated, the fragmented and
// the ones with a 0x02 sitting in the middle of a payload.

#include "gsof/framer.h"
#include "gsof/trimcomm.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

using namespace gsof;

// ============================================================================
// Compile-time: the packet format itself
// ============================================================================

// A minimal GENOUT packet carrying one three-byte transport header and no
// records. Hand-computed from the ICD so that it checks the implementation
// rather than agreeing with it:
//   STX  STATUS TYPE LEN  TX  PG  MAX  CSUM ETX
//   02   00     40   03   07  00  00   4A   03
// checksum = 0x00 + 0x40 + 0x03 + 0x07 + 0x00 + 0x00 = 0x4A
constexpr std::array<std::uint8_t, 9> kMinimalGenOut {
    0x02, 0x00, 0x40, 0x03, 0x07, 0x00, 0x00, 0x4A, 0x03
};

static_assert(trimcomm::parse_packet(kMinimalGenOut).has_value(),
              "the hand-computed GENOUT packet must validate");
static_assert(trimcomm::parse_packet(kMinimalGenOut)->type == 0x40);
static_assert(trimcomm::parse_packet(kMinimalGenOut)->is(trimcomm::PacketType::GenOut));
static_assert(trimcomm::parse_packet(kMinimalGenOut)->data.size() == 3);
static_assert(trimcomm::parse_packet(kMinimalGenOut)->data[0] == 0x07);

// The checksum is over STATUS, TYPE, LENGTH and DATA -- not STX, not ETX. If
// this ever agrees with a sum that includes STX it will read 0x4C.
static_assert(trimcomm::checksum(0x00, 0x40, std::span<const std::uint8_t>(kMinimalGenOut).subspan(4, 3)) == 0x4A);

// It is defined modulo 256, so a payload that carries the accumulator past 255
// must wrap rather than saturate or widen.
//   0x00 + 0x00 + len(3) + 0xFF + 0xFF + 0x10 = 0x211, and only the low byte
//   is the checksum.
constexpr std::array<std::uint8_t, 3> kWrapping { 0xFF, 0xFF, 0x10 };
static_assert(trimcomm::checksum(0x00, 0x00, kWrapping) == 0x11);

// Every way of being malformed, at compile time.
static_assert(trimcomm::parse_packet(std::span<const std::uint8_t>()).error().kind == ErrorKind::Truncated);
static_assert(trimcomm::parse_packet(std::span<const std::uint8_t>(kMinimalGenOut).first(5)).error().kind ==
                  ErrorKind::Truncated,
              "too few bytes to read the header at all");
static_assert(trimcomm::parse_packet(std::span<const std::uint8_t>(kMinimalGenOut).first(7)).error().kind ==
                  ErrorKind::Truncated,
              "header read, body still arriving -- Truncated, not BadFraming, because the framer WAITS on "
              "this one and resynchronises on everything else");

constexpr std::array<std::uint8_t, 9> withByteChanged(std::size_t index, std::uint8_t value)
{
    std::array<std::uint8_t, 9> copy = kMinimalGenOut;
    copy[index] = value;
    return copy;
}

constexpr std::array<std::uint8_t, 9> kNoStx = withByteChanged(0, 0x00);
constexpr std::array<std::uint8_t, 9> kNoEtx = withByteChanged(8, 0x00);
constexpr std::array<std::uint8_t, 9> kBadSum = withByteChanged(7, 0x4B);
constexpr std::array<std::uint8_t, 9> kPayloadChanged = withByteChanged(4, 0x08);

static_assert(trimcomm::parse_packet(kNoStx).error().kind == ErrorKind::BadFraming);
static_assert(trimcomm::parse_packet(kNoEtx).error().kind == ErrorKind::BadFraming);
static_assert(trimcomm::parse_packet(kBadSum).error().kind == ErrorKind::BadChecksum);
// Changing a payload byte must break the checksum. If it does not, the
// checksum is not covering DATA.
static_assert(trimcomm::parse_packet(kPayloadChanged).error().kind == ErrorKind::BadChecksum);

// Build and parse must agree, at compile time, in both directions.
constexpr std::array<std::uint8_t, 3> kTransportOnly { 0x07, 0x00, 0x00 };
constexpr auto kBuilt = trimcomm::make_packet(trimcomm::PacketType::GenOut, kTransportOnly);

static_assert(kBuilt.size() == kMinimalGenOut.size());
static_assert(kBuilt == kMinimalGenOut, "make_packet must produce the hand-computed bytes exactly");
static_assert(trimcomm::parse_packet(kBuilt).has_value());

// packet_size is the arithmetic every stream reader depends on to know when it
// has enough bytes; off by one here is off by one everywhere.
static_assert(trimcomm::packet_size(0) == 6);
static_assert(trimcomm::packet_size(255) == 261);
static_assert(trimcomm::packet_size(255) == trimcomm::kMaxPacketSize);

// ============================================================================
// Run-time helpers
// ============================================================================

// Build a packet with an arbitrary-length payload, for the framer tests.
std::vector<std::uint8_t> makePacket(trimcomm::PacketType type, const std::vector<std::uint8_t>& data,
                                     std::uint8_t status = 0)
{
    std::vector<std::uint8_t> out(data.size() + trimcomm::kOverheadSize);
    const Result<std::size_t> written = trimcomm::encode_packet(type, data, out, status);
    check(written.has_value(), "encode_packet into an exactly-sized buffer");
    return out;
}

std::vector<trimcomm::PacketView> drain(Framer& framer)
{
    std::vector<trimcomm::PacketView> found;
    while (const auto packet = framer.next())
    {
        found.push_back(*packet);
    }
    return found;
}

// ============================================================================
// encode_packet
// ============================================================================

void test_encode_matches_parse()
{
    const std::vector<std::uint8_t> payload { 0x01, 0x02, 0x03, 0x04, 0xFF };
    const std::vector<std::uint8_t> packet = makePacket(trimcomm::PacketType::AppFile, payload);

    const Result<trimcomm::PacketView> parsed = trimcomm::parse_packet(packet);
    check(parsed.has_value(), "a packet built by encode_packet parses");
    if (parsed.has_value())
    {
        check(parsed->is(trimcomm::PacketType::AppFile), "round trip preserves the type");
        check(std::vector<std::uint8_t>(parsed->data.begin(), parsed->data.end()) == payload,
              "round trip preserves the payload");
    }

    // A buffer one byte short must be refused rather than half-written.
    std::vector<std::uint8_t> tooSmall(payload.size() + trimcomm::kOverheadSize - 1);
    const Result<std::size_t> refused = trimcomm::encode_packet(trimcomm::PacketType::AppFile, payload, tooSmall);
    check(!refused.has_value() && refused.error().kind == ErrorKind::Truncated,
          "encode_packet refuses a buffer that is too small");

    // The status byte is carried and included in the checksum.
    const std::vector<std::uint8_t> stamped = makePacket(trimcomm::PacketType::AppFile, payload, 0x5A);
    const Result<trimcomm::PacketView> withStatus = trimcomm::parse_packet(stamped);
    check(withStatus.has_value() && withStatus->status == 0x5A, "the status byte round-trips");
}

// ============================================================================
// Framer: the happy paths
// ============================================================================

void test_framer_finds_back_to_back_packets()
{
    Framer framer;

    std::vector<std::uint8_t> stream;
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        const std::vector<std::uint8_t> packet = makePacket(trimcomm::PacketType::GenOut, { i, 0x00, 0x00, 0xAA });
        stream.insert(stream.end(), packet.begin(), packet.end());
    }

    framer.push(stream);
    const std::vector<trimcomm::PacketView> found = drain(framer);

    check(found.size() == 5, "five packets in, five packets out");
    for (std::size_t i = 0; i < found.size(); ++i)
    {
        check(found[i].data.size() == 4 && found[i].data[0] == static_cast<std::uint8_t>(i),
              "packets come out in order with their payloads intact");
    }
    check(framer.stats().resyncs == 0, "a clean stream needs no resynchronisation");
    check(framer.buffered() == 0, "a stream of whole packets leaves nothing behind");
}

void test_framer_is_chunk_boundary_agnostic()
{
    // The stream chopped into every possible fragmentation, driven by a fixed
    // sequence rather than a random one so a failure is reproducible. This is
    // the case TCP actually produces and the one a length-prefixed parser
    // written against whole reads gets wrong.
    std::vector<std::uint8_t> stream;
    std::vector<std::uint8_t> expectedFirstBytes;
    for (std::uint8_t i = 0; i < 20; ++i)
    {
        const std::vector<std::uint8_t> packet =
            makePacket(trimcomm::PacketType::GenOut, std::vector<std::uint8_t>(1 + (i % 17), i));
        stream.insert(stream.end(), packet.begin(), packet.end());
        expectedFirstBytes.push_back(i);
    }

    for (std::size_t chunk = 1; chunk <= 25; ++chunk)
    {
        Framer framer;
        std::vector<std::uint8_t> firstBytes;

        for (std::size_t offset = 0; offset < stream.size(); offset += chunk)
        {
            const std::size_t take = std::min(chunk, stream.size() - offset);
            framer.push(std::span<const std::uint8_t>(stream.data() + offset, take));
            while (const auto packet = framer.next())
            {
                firstBytes.push_back(packet->data[0]);
            }
        }

        check(firstBytes == expectedFirstBytes,
              "every packet is recovered when the stream arrives in " + std::to_string(chunk) + "-byte chunks");
        check(framer.stats().resyncs == 0,
              "fragmentation alone never causes a resync at chunk size " + std::to_string(chunk));
    }
}

void test_stx_inside_a_payload_is_not_a_frame_start()
{
    // 0x02 is an ordinary data byte. A framer that scans for STX without
    // honouring the length field splits this packet in the middle.
    Framer framer;
    const std::vector<std::uint8_t> packet =
        makePacket(trimcomm::PacketType::GenOut, { 0x02, 0x02, 0x03, 0x02, 0x03, 0x02 });

    framer.push(packet);
    const std::vector<trimcomm::PacketView> found = drain(framer);

    check(found.size() == 1, "a payload full of STX and ETX bytes is still one packet");
    check(framer.stats().resyncs == 0, "and needs no resynchronisation");
}

// ============================================================================
// Framer: recovery
// ============================================================================

void test_framer_recovers_from_a_corrupted_packet()
{
    // Three packets, the middle one with a flipped payload byte so its
    // checksum fails. The third must still come out: this is precisely the
    // case the reference driver wedges on.
    std::vector<std::uint8_t> good1 = makePacket(trimcomm::PacketType::GenOut, { 0x11, 0x22, 0x33 });
    std::vector<std::uint8_t> bad = makePacket(trimcomm::PacketType::GenOut, { 0x44, 0x55, 0x66 });
    std::vector<std::uint8_t> good2 = makePacket(trimcomm::PacketType::GenOut, { 0x77, 0x88, 0x99 });

    bad[5] ^= 0xFF;

    std::vector<std::uint8_t> stream;
    stream.insert(stream.end(), good1.begin(), good1.end());
    stream.insert(stream.end(), bad.begin(), bad.end());
    stream.insert(stream.end(), good2.begin(), good2.end());

    Framer framer;
    framer.push(stream);
    const std::vector<trimcomm::PacketView> found = drain(framer);

    check(found.size() == 2, "the two intact packets are recovered around the corrupted one");
    if (found.size() == 2)
    {
        check(found[0].data[0] == 0x11 && found[1].data[0] == 0x77,
              "and they are the right two, in order");
    }
    check(framer.stats().checksumErrors == 1, "the checksum failure is counted exactly once");
    check(framer.stats().resyncs >= 1, "and it caused a resynchronisation");
    check(framer.stats().droppedBytes > 0, "which discarded the bytes it could not use");
}

void test_framer_recovers_from_leading_garbage()
{
    Framer framer;

    std::vector<std::uint8_t> stream { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11 };
    const std::vector<std::uint8_t> packet = makePacket(trimcomm::PacketType::GenOut, { 0x01, 0x02 });
    stream.insert(stream.end(), packet.begin(), packet.end());

    framer.push(stream);
    const std::vector<trimcomm::PacketView> found = drain(framer);

    check(found.size() == 1, "a packet is found after six bytes of junk");
    check(framer.stats().droppedBytes == 6, "and exactly the junk was dropped");
    // Junk with no STX in it should cost ONE resync, not one per byte --
    // otherwise the counter reports the size of the garbage rather than the
    // number of times the stream broke.
    check(framer.stats().resyncs == 1, "a contiguous run of junk is one resync event");
}

void test_framer_recovers_when_etx_is_wrong()
{
    // A length byte that points somewhere ETX is not. The packet is
    // unrecoverable, the next one must not be.
    //
    // The payload deliberately contains no 0x02, so the resync lands directly
    // on the following packet and the framing-error count is exactly one. With
    // an STX byte inside the payload the scanner would stop there first and
    // report a second framing error -- correct behaviour, but it would make
    // this assertion about the scanner rather than about recovery.
    std::vector<std::uint8_t> bad = makePacket(trimcomm::PacketType::GenOut, { 0x11, 0x22, 0x33 });
    bad.back() = 0x00;
    const std::vector<std::uint8_t> good = makePacket(trimcomm::PacketType::GenOut, { 0x0A, 0x0B, 0x0C });

    std::vector<std::uint8_t> stream = bad;
    stream.insert(stream.end(), good.begin(), good.end());

    Framer framer;
    framer.push(stream);
    const std::vector<trimcomm::PacketView> found = drain(framer);

    check(found.size() == 1 && found[0].data[0] == 0x0A, "the packet after a bad ETX is recovered");
    check(framer.stats().framingErrors == 1, "a misplaced ETX is counted as a framing error");
}

void test_framer_waits_rather_than_resyncing_on_a_partial_packet()
{
    Framer framer;
    const std::vector<std::uint8_t> packet = makePacket(trimcomm::PacketType::GenOut, { 0x01, 0x02, 0x03 });

    framer.push(std::span<const std::uint8_t>(packet.data(), packet.size() - 1));
    check(framer.next() == std::nullopt, "an incomplete packet yields nothing");
    check(framer.stats().resyncs == 0, "and is NOT treated as corruption -- it is the normal TCP case");
    check(framer.stats().droppedBytes == 0, "so nothing is discarded while waiting");

    framer.push(std::span<const std::uint8_t>(packet.data() + packet.size() - 1, 1));
    check(framer.next().has_value(), "the packet completes when its last byte arrives");
}

void test_framer_bounds_its_buffer()
{
    // A far end that is not speaking DCOL at all. The buffer must not grow
    // with the stream.
    Framer framer(1024);

    const std::vector<std::uint8_t> junk(4096, 0xAB);
    framer.push(junk);
    check(framer.next() == std::nullopt, "no packet is found in junk");
    check(framer.buffered() <= 1024, "the buffer stays inside its cap");
    check(framer.stats().overflows >= 1, "and the overflow is counted rather than silent");

    // And it still works afterwards.
    const std::vector<std::uint8_t> packet = makePacket(trimcomm::PacketType::GenOut, { 0x01 });
    framer.push(packet);
    check(framer.next().has_value(), "a real packet is still found after an overflow");
}

void test_framer_reset_drops_everything()
{
    Framer framer;
    const std::vector<std::uint8_t> packet = makePacket(trimcomm::PacketType::GenOut, { 0x01, 0x02, 0x03 });

    framer.push(std::span<const std::uint8_t>(packet.data(), 4));
    framer.reset();
    check(framer.buffered() == 0, "reset empties the buffer");

    // Half a packet from before a dropped connection must not be able to
    // combine with bytes from after it.
    framer.push(std::span<const std::uint8_t>(packet.data() + 4, packet.size() - 4));
    check(framer.next() == std::nullopt, "the tail of a pre-reset packet cannot form a packet on its own");
}

void test_maximum_size_packet()
{
    Framer framer;
    const std::vector<std::uint8_t> packet =
        makePacket(trimcomm::PacketType::GenOut, std::vector<std::uint8_t>(trimcomm::kMaxDataSize, 0x5A));

    check(packet.size() == trimcomm::kMaxPacketSize, "a full-length packet is 261 bytes");

    framer.push(packet);
    const std::vector<trimcomm::PacketView> found = drain(framer);
    check(found.size() == 1 && found[0].data.size() == trimcomm::kMaxDataSize,
          "a full-length packet round-trips through the framer");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    test_encode_matches_parse();
    test_framer_finds_back_to_back_packets();
    test_framer_is_chunk_boundary_agnostic();
    test_stx_inside_a_payload_is_not_a_frame_start();
    test_framer_recovers_from_a_corrupted_packet();
    test_framer_recovers_from_leading_garbage();
    test_framer_recovers_when_etx_is_wrong();
    test_framer_waits_rather_than_resyncing_on_a_partial_packet();
    test_framer_bounds_its_buffer();
    test_framer_reset_drops_everything();
    test_maximum_size_packet();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all GSOF framing checks passed");
    return 0;
}
