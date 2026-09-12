// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MTData2 body: the item walk, and the fifteen payloads an MTi-610 emits.
//
// Every vector here is SYNTHETIC. No MTi has been on the bench, so none of
// this can confirm the wire format -- it confirms that the parser is
// self-consistent, that it sizes payloads from the format nibble rather than
// from a constant, and that it refuses malformed input instead of decoding it
// into a plausible number. See tests/golden/README.md for what would have to
// change once hardware exists.
//
// The one assertion here that is stronger than self-consistency is
// testSameValueInEveryPrecision(): one acceleration, encoded in all four
// formats, must parse to the same three numbers. That is the item-level form
// of the argument test_fixed_point.cpp makes, and it is what catches a payload
// sized by a constant.

#include "xbus/framer.h"
#include "xbus/mtdata2.h"

#include "golden/golden_messages.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
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

constexpr bool near(double a, double b, double tolerance)
{
    const double d = a - b;
    return (d < 0 ? -d : d) <= tolerance;
}

// Append one item to an MTData2 body under construction.
void appendItem(std::vector<std::uint8_t>& body, std::uint16_t rawId,
                std::span<const std::uint8_t> payload)
{
    body.push_back(static_cast<std::uint8_t>(rawId >> 8));
    body.push_back(static_cast<std::uint8_t>(rawId & 0xFF));
    body.push_back(static_cast<std::uint8_t>(payload.size()));
    body.insert(body.end(), payload.begin(), payload.end());
}

// A three-component payload in whichever precision the caller names. This is
// the encoder the format-nibble tests rest on, and it is deliberately the one
// in byte_order.h rather than a local copy -- a test-local encoder that got
// fp16.32 wrong in the same way as the reader would agree with it.
std::vector<std::uint8_t> encodeReals(std::span<const double> values, Precision precision)
{
    std::vector<std::uint8_t> out(values.size() * precision_size(precision), 0);
    const std::span<std::uint8_t> span(out);

    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const std::size_t at = i * precision_size(precision);
        switch (precision)
        {
            case Precision::Float32: write_f32(span, at, static_cast<float>(values[i])); break;
            case Precision::Fp1220:  write_fp1220(span, at, values[i]); break;
            case Precision::Fp1632:  write_fp1632(span, at, values[i]); break;
            case Precision::Float64:
            {
                // No write_f64 in byte_order.h: nothing this library encodes
                // uses it. Assembling it here keeps that true.
                const std::uint64_t raw = std::bit_cast<std::uint64_t>(values[i]);
                for (std::size_t b = 0; b < 8; ++b)
                {
                    out[at + b] = static_cast<std::uint8_t>((raw >> (8 * (7 - b))) & 0xFFu);
                }
                break;
            }
        }
    }

    return out;
}

std::uint16_t withPrecision(DataId id, Precision precision)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(id) |
                                      static_cast<std::uint16_t>(precision));
}

// ============================================================================
// Compile-time: the identifier's three fields
// ============================================================================

// 0x4020 and 0x4022 are the SAME measurement in different formats. A parser
// that compared all sixteen bits would decode one and drop the other with no
// error anywhere, which is the reason kFullTypeMask exists.
static_assert(data_type(0x4022) == static_cast<std::uint16_t>(DataId::Acceleration));
static_assert(data_type(0x4020) == data_type(0x4022));
static_assert(data_precision(0x4022) == Precision::Fp1632);
static_assert(data_precision(0x4020) == Precision::Float32);
static_assert(data_group(0x4022) == DataGroup::Acceleration);

// Table 18's worked example: "Quaternion orientation output (201y) expressed
// in the NED coordinate system with fixed point 16.32 numbers ... the
// resulting hex value for the identifier will be 0x2016." Orientation is not
// something a 610 produces, but the bit arithmetic is shared with everything
// that is, and this is the LLCP's own decomposition of it.
static_assert(data_precision(0x2016) == Precision::Fp1632, "0x6 = Fp16.32 | NED");
static_assert(data_coordinate_system(0x2016) == 0x4, "and the NED half");
static_assert(data_group(0x2016) == DataGroup::Orientation);

static_assert(precision_size(Precision::Float32) == 4);
static_assert(precision_size(Precision::Fp1220) == 4);
static_assert(precision_size(Precision::Fp1632) == 6, "six bytes, not eight");
static_assert(precision_size(Precision::Float64) == 8);

static_assert(is_known_data_id(0x4022), "a known identifier in a non-default format");
static_assert(!is_known_data_id(0x2010), "orientation: an MTi-610 does not produce it");
static_assert(!is_known_data_id(0x4030), "free acceleration: needs the filter a 610 lacks");
static_assert(!is_known_data_id(0x7010), "GNSS PVT");

// ============================================================================
// Compile-time: the item walk, against the framing test's own vector
// ============================================================================

constexpr std::optional<DataItem> firstItem(std::span<const std::uint8_t> body)
{
    ItemIterator it(body);
    return it.next();
}

// kMtData2PacketCounter's body is 10 20 02 04 D2.
constexpr std::array<std::uint8_t, 5> kCounterBody { 0x10, 0x20, 0x02, 0x04, 0xD2 };
static_assert(firstItem(kCounterBody).has_value());
static_assert(firstItem(kCounterBody)->rawId == 0x1020);
static_assert(firstItem(kCounterBody)->payload.size() == 2);
static_assert(PacketCounter::parse(*firstItem(kCounterBody))->counter == 1234);

// The status word from the framing test's preamble-in-payload vector, which is
// 0xFA00FA01 -- chosen there to put preamble bytes inside a payload, and used
// here because a value with bits scattered across all four bytes is what shows
// up a byte-order mistake.
constexpr std::array<std::uint8_t, 7> kStatusBody { 0xE0, 0x20, 0x04, 0xFA, 0x00, 0xFA, 0x01 };
static_assert(StatusWord::parse(*firstItem(kStatusBody))->status == 0xFA00FA01);

constexpr StatusWord kStatus = *StatusWord::parse(*firstItem(kStatusBody));
static_assert(kStatus.selfTestPassed(), "bit 0 of 0x...01");
static_assert(!kStatus.filterValid(), "bit 1 is clear");
static_assert(kStatus.clipMagY(), "bit 15 is set in 0xFA00FA01");
static_assert(!kStatus.clipping(), "bit 19 is clear");

// A length that is not what the identifier calls for is a hard error. Unlike a
// GSOF record, an MTData2 item cannot legitimately grow: accepting a long one
// would mean parsing the NEXT item from the wrong offset.
static_assert(
    PacketCounter::parse(DataItem { 0x1020, std::span<const std::uint8_t>(kCounterBody.data(), 4) })
        .error()
        .kind == ErrorKind::LengthMismatch);

// ============================================================================
// Run time
// ============================================================================

void testSameValueInEveryPrecision()
{
    // The assertion this file is built around. One acceleration, sent four
    // ways, must parse to one answer. A payload sized by a constant rather
    // than by the format nibble fails here; so does any fixed-point byte
    // order that disagrees with float32.
    const double values[] = { 9.81, -0.42, 0.0 };

    Acceleration reference {};
    bool haveReference = false;

    for (Precision precision : { Precision::Float32, Precision::Fp1220,
                                 Precision::Fp1632, Precision::Float64 })
    {
        const std::vector<std::uint8_t> payload = encodeReals(values, precision);
        const DataItem item { withPrecision(DataId::Acceleration, precision), payload };

        const Result<Acceleration> parsed = Acceleration::parse(item);
        check(parsed.has_value(),
              std::string("acceleration parses as ") + to_string(precision));

        if (!parsed) { continue; }

        check(payload.size() == 3 * precision_size(precision),
              std::string("payload is sized by the format nibble: ") + to_string(precision));

        if (!haveReference)
        {
            reference = *parsed;
            haveReference = true;
            continue;
        }

        // fp12.20 is the coarsest of the four at this magnitude.
        check(near(parsed->xMps2, reference.xMps2, 1e-4) &&
                  near(parsed->yMps2, reference.yMps2, 1e-4) &&
                  near(parsed->zMps2, reference.zMps2, 1e-4),
              std::string("every precision decodes the same acceleration: ") + to_string(precision));
    }

    check(near(reference.xMps2, 9.81, 1e-6), "and it is the value that was encoded");
}

void testWalkAMixedPacket()
{
    // What one sample actually looks like: the packet metadata and several
    // measurements, in one message, in the order the device happened to emit
    // them.
    const double acc[] = { 0.1, 0.2, 9.8 };
    const double gyr[] = { 0.01, -0.02, 0.03 };

    std::vector<std::uint8_t> body;
    appendItem(body, 0x1020, std::array<std::uint8_t, 2> { 0x00, 0x07 });
    appendItem(body, 0x1060, std::array<std::uint8_t, 4> { 0x00, 0x01, 0x86, 0xA0 });
    appendItem(body, withPrecision(DataId::Acceleration, Precision::Fp1632),
               encodeReals(acc, Precision::Fp1632));
    appendItem(body, withPrecision(DataId::RateOfTurn, Precision::Float32),
               encodeReals(gyr, Precision::Float32));
    appendItem(body, 0xE020, std::array<std::uint8_t, 4> { 0x00, 0x00, 0x00, 0x03 });

    ItemIterator it(body);
    std::vector<std::uint16_t> seen;
    while (const std::optional<DataItem> item = it.next())
    {
        seen.push_back(item->type());
    }

    check(it.ok(), "a well-formed body walks to the end without error");
    check(seen.size() == 5, "five items");
    check(seen == std::vector<std::uint16_t> { 0x1020, 0x1060, 0x4020, 0x8020, 0xE020 },
          "in the order they were written, identified by type regardless of format");
}

void testVisitorDispatch()
{
    const double mag[] = { 0.3, -0.9, 0.2 };

    std::vector<std::uint8_t> body;
    appendItem(body, withPrecision(DataId::MagneticField, Precision::Float32),
               encodeReals(mag, Precision::Float32));
    // Something a 610 does not send: free acceleration. Not an error -- an
    // item is self-delimiting, so an unmodelled one is skippable, and the node
    // publishes it as raw bytes rather than dropping it silently.
    appendItem(body, 0x4030, std::array<std::uint8_t, 12> {});

    int magnetic = 0;
    int unmodelled = 0;
    double magX = 0.0;

    ItemIterator it(body);
    while (const std::optional<DataItem> item = it.next())
    {
        visit_item(*item, [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, DataItem>)
            {
                ++unmodelled;
                check(value.rawId == 0x4030, "the unmodelled item arrives whole");
            }
            else if constexpr (std::is_same_v<T, Result<MagneticField>>)
            {
                ++magnetic;
                if (value) { magX = value->xAu; }
            }
        });
    }

    check(magnetic == 1, "the magnetic item dispatched to its own parser");
    check(unmodelled == 1, "and the unmodelled one fell through to the raw path");
    check(near(magX, 0.3, 1e-6), "with the right value");
}

void testMalformedItems()
{
    // An item header that does not fit.
    {
        const std::array<std::uint8_t, 2> body { 0x10, 0x20 };
        ItemIterator it(body);
        check(!it.next().has_value(), "a body too short for an item header yields nothing");
        check(!it.ok() && it.error()->kind == ErrorKind::Truncated, "and says why");
    }

    // A length byte claiming more payload than the body holds. The walk has to
    // stop: every offset after a wrong length is meaningless.
    {
        const std::array<std::uint8_t, 5> body { 0x40, 0x20, 0x40, 0x00, 0x00 };
        ItemIterator it(body);
        check(!it.next().has_value(), "an item longer than its body yields nothing");
        check(!it.ok() && it.error()->kind == ErrorKind::Truncated, "and says why");
    }

    // A good item followed by a malformed one: the good one still comes out.
    {
        std::vector<std::uint8_t> body;
        appendItem(body, 0x1020, std::array<std::uint8_t, 2> { 0x00, 0x01 });
        body.insert(body.end(), { 0x40, 0x20, 0x40 });

        ItemIterator it(body);
        check(it.next().has_value(), "the item before the damage is delivered");
        check(!it.next().has_value(), "and the walk stops at the damage");
        check(!it.ok(), "with the reason recorded");
    }
}

void testCoordinateSystemBitIsRefused()
{
    // Only the orientation and velocity groups define these bits, and a 610
    // produces neither. A nonzero value means the identifier is not what we
    // think it is, so decoding it anyway would turn a misread into three
    // plausible numbers.
    const double acc[] = { 1.0, 2.0, 3.0 };
    const std::vector<std::uint8_t> payload = encodeReals(acc, Precision::Float32);

    const DataItem item { 0x4024, payload };  // NED bit set on acceleration
    const Result<Acceleration> parsed = Acceleration::parse(item);

    check(!parsed.has_value(), "a coordinate-system bit on acceleration is refused");
    check(!parsed && parsed.error().kind == ErrorKind::UnsupportedFormat, "as unsupported");
}

void testFixedLayoutPayloads()
{
    // UTC time: ns U4, year U2, then six single bytes. 2026-09-11 19:35:28.
    const std::array<std::uint8_t, 12> utc {
        0x00, 0x00, 0x00, 0x64,  // 100 ns
        0x07, 0xEA,              // 2026
        0x09, 0x0B,              // September 11
        0x13, 0x23, 0x1C,        // 19:35:28
        0x07                     // date, time and resolved all valid
    };

    const Result<UtcTime> parsed = UtcTime::parse(DataItem { 0x1010, utc });
    check(parsed.has_value(), "UTC time parses");
    if (parsed)
    {
        check(parsed->year == 2026 && parsed->month == 9 && parsed->day == 11,
              "with the date at the right offsets");
        check(parsed->hour == 19 && parsed->minute == 35 && parsed->second == 28,
              "and the time");
        check(parsed->nanosecond == 100, "and the nanoseconds ahead of the year, not behind it");
        check(parsed->dateValid() && parsed->timeValid() && parsed->fullyResolved(),
              "and all three validity bits");
    }

    const Result<BaroPressure> baro =
        BaroPressure::parse(DataItem { 0x3010, std::array<std::uint8_t, 4> { 0x00, 0x01, 0x8B, 0x32 } });
    check(baro.has_value() && baro->pressurePa == 101170, "barometric pressure is whole pascals");

    const Result<StatusByte> status =
        StatusByte::parse(DataItem { 0xE010, std::array<std::uint8_t, 1> { 0x03 } });
    check(status.has_value() && status->status == 0x03, "the status byte");

    const Result<SampleTimeFine> fine =
        SampleTimeFine::parse(DataItem { 0x1060, std::array<std::uint8_t, 4> { 0x00, 0x01, 0x86, 0xA0 } });
    check(fine.has_value() && fine->ticks == 100000, "sample time fine, in 10 kHz ticks");
    check(near(static_cast<double>(fine->ticks) / SampleTimeFine::kTicksPerSecond, 10.0, 1e-9),
          "which is ten seconds");
}

void testClippingBitsAgreeWithTheSummary()
{
    // Table 27 bit 19 is set when any of bits 8..16 is. A synthetic vector
    // cannot prove the device honours that, but it can prove our decode of the
    // two halves is consistent -- and it is the pair a consumer watches.
    const StatusWord clipped { 0x00080700u };  // bits 8,9,10 (acc clip) + bit 19 (summary)
    check(clipped.clipping(), "bit 19 reads as the clipping summary");
    check(clipped.clipAccX() && clipped.clipAccY() && clipped.clipAccZ(),
          "and the individual accelerometer axes below it");

    const StatusWord clean { 0x00000003u };
    check(!clean.clipping() && !clean.clipAccX(), "a clean word reports neither");
    check(clean.selfTestPassed() && clean.filterValid(), "and the two bits it does set");
}

void testMtData2FromTheFramer()
{
    // End to end at the library's own seam: bytes in, parsed measurement out.
    Framer framer;
    framer.push(bytes(golden::kMtData2WithPreambleInPayload));

    const std::optional<MessageView> message = framer.next();
    check(message.has_value() && message->is(MessageId::MtData2), "the framer yields an MTData2");

    if (!message) { return; }

    ItemIterator it(message->data);
    const std::optional<DataItem> item = it.next();
    check(item.has_value() && item->is(DataId::StatusWord), "whose single item is a status word");

    if (item)
    {
        const Result<StatusWord> status = StatusWord::parse(*item);
        check(status.has_value() && status->status == 0xFA00FA01,
              "and the preamble bytes inside its payload survived the framing");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testSameValueInEveryPrecision();
    testWalkAMixedPacket();
    testVisitorDispatch();
    testMalformedItems();
    testCoordinateSystemBitIsRefused();
    testFixedLayoutPayloads();
    testClippingBitsAgreeWithTheSummary();
    testMtData2FromTheFramer();

    return failures == 0 ? 0 : 1;
}
