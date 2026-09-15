// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MTData2 message body: a sequence of self-delimiting items,
//
//     DATA_ID(2) | LEN(1) | PAYLOAD[LEN]
//
// repeated to the end of the message. One MTData2 is one SAMPLE, and the items
// in it belong together: the packet counter, the sample time and the status
// word are properties of the packet, shared by every measurement beside them.
// Nothing in this file fuses them -- that is the node's job -- but it is why
// ItemIterator hands out items rather than a flat list of values.
//
// Layouts come from the LLCP (MT0101P rev 2019.C, section 5.3.6). Conventions,
// all of them load-bearing:
//
//   * FIELD NAMES CARRY UNITS, and the units are the WIRE's. Magnetic field is
//     in arbitrary units because that is what the device outputs; calling it
//     microtesla here would be a lie that survives into the schema. The node
//     converts, once, on the way to capnp.
//
//   * REAL-VALUED PAYLOADS ARE SIZED BY THE FORMAT NIBBLE, not by a constant.
//     Acceleration is twelve bytes as float32 and eighteen as fp16.32, and a
//     parser with a hardcoded size decodes one and silently drops the other.
//
//   * AN ITEM OF THE WRONG LENGTH IS A HARD ERROR, unlike a GSOF record, which
//     may legitimately grow. An MTData2 item's length is fully determined by
//     its identifier, so a mismatch means the identifier was misread -- and
//     accepting it would parse the next item from the wrong offset.
//
//   * EVERY VALUE IS WIDENED TO double. The device may send float32 or fp16.32
//     for the same quantity depending on configuration, and a struct whose
//     member type changed with the configuration would push that choice into
//     every consumer.

#ifndef XBUS_MTDATA2_H
#define XBUS_MTDATA2_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "xbus/byte_order.h"
#include "xbus/data_id.h"
#include "xbus/error.h"

namespace xbus
{

// One item, still unparsed. `payload` points into the message it came from.
struct DataItem
{
    // The identifier exactly as it arrived, format nibble included.
    std::uint16_t rawId { 0 };

    std::span<const std::uint8_t> payload;

    constexpr std::uint16_t type() const { return data_type(rawId); }
    constexpr DataGroup group() const { return data_group(rawId); }
    constexpr Precision precision() const { return data_precision(rawId); }
    constexpr std::uint8_t coordinateSystem() const { return data_coordinate_system(rawId); }

    constexpr bool is(DataId id) const
    {
        return type() == static_cast<std::uint16_t>(id);
    }

    // The low byte of the identifier: its type and format nibbles, which is
    // the half that says which parser was being asked for. Used as an Error's
    // `detail`, which is only eight bits wide.
    constexpr std::uint8_t detail() const { return static_cast<std::uint8_t>(rawId & 0x00FF); }
};

// Walks the items in an MTData2 body.
//
// Not an iterator in the standard sense -- next() returns nullopt at the end
// and the walk stops on the first malformed item, because an item whose length
// byte is wrong makes every offset after it meaningless. `error()` says why a
// walk stopped short; a walk that consumed the whole body leaves it empty.
class ItemIterator
{
  public:
    explicit constexpr ItemIterator(std::span<const std::uint8_t> body) : mBody(body) {}

    static constexpr std::size_t kItemHeaderSize = 3;

    constexpr std::optional<DataItem> next()
    {
        if (mOffset >= mBody.size())
        {
            return std::nullopt;
        }

        if (mBody.size() - mOffset < kItemHeaderSize)
        {
            mError = Error { ErrorKind::Truncated, 0, static_cast<std::uint16_t>(mOffset) };
            mOffset = mBody.size();
            return std::nullopt;
        }

        const std::uint16_t rawId = read_u16(mBody, mOffset);
        const std::size_t length = read_u8(mBody, mOffset + 2);

        if (mBody.size() - mOffset - kItemHeaderSize < length)
        {
            mError = Error { ErrorKind::Truncated,
                             static_cast<std::uint8_t>(rawId & 0x00FF),
                             static_cast<std::uint16_t>(mOffset) };
            mOffset = mBody.size();
            return std::nullopt;
        }

        const DataItem item { rawId, mBody.subspan(mOffset + kItemHeaderSize, length) };
        mOffset += kItemHeaderSize + length;
        return item;
    }

    constexpr const std::optional<Error>& error() const { return mError; }
    constexpr bool ok() const { return !mError.has_value(); }

  private:
    std::span<const std::uint8_t> mBody;
    std::size_t mOffset { 0 };
    std::optional<Error> mError;
};

namespace detail
{

// One real-valued component, in whichever of the four formats the identifier
// selected. The fp16.32 case is the one that matters -- see read_fp1632().
constexpr double read_real(std::span<const std::uint8_t> bytes, std::size_t offset,
                           Precision precision)
{
    switch (precision)
    {
        case Precision::Float32: return static_cast<double>(read_f32(bytes, offset));
        case Precision::Fp1220:  return read_fp1220(bytes, offset);
        case Precision::Fp1632:  return read_fp1632(bytes, offset);
        case Precision::Float64: return read_f64(bytes, offset);
    }

    return 0.0;
}

// The length check every real-valued parser starts with, plus the two things
// that make a real-valued item different from a fixed-layout one: its size
// depends on the format nibble, and a coordinate-system bit on a group that
// has none means the identifier was misread.
template <std::size_t N>
constexpr Result<std::array<double, N>> parse_reals(const DataItem& item)
{
    if (item.coordinateSystem() != 0)
    {
        // Only the orientation and velocity groups define these bits, and an
        // MTi-610 produces neither. Refusing is the point: a nonzero value
        // here means the identifier is not what we think it is.
        return unsupported_format(item.detail());
    }

    const std::size_t need = N * precision_size(item.precision());

    if (item.payload.size() < need)
    {
        return truncated(static_cast<std::uint16_t>(item.payload.size()), item.detail());
    }

    if (item.payload.size() != need)
    {
        return length_mismatch(item.detail(), static_cast<std::uint16_t>(item.payload.size()));
    }

    std::array<double, N> out {};
    const std::size_t stride = precision_size(item.precision());
    for (std::size_t i = 0; i < N; ++i)
    {
        out[i] = read_real(item.payload, i * stride, item.precision());
    }
    return out;
}

// The same check for an item whose payload is a fixed binary layout. The
// format nibble is ignored for these: the LLCP gives them a settable format
// field but a payload defined as U2/U4 regardless, and MT Manager writes zero.
template <typename T>
constexpr std::optional<Result<T>> require(const DataItem& item, std::size_t need)
{
    if (item.payload.size() == need)
    {
        return std::nullopt;
    }

    if (item.payload.size() < need)
    {
        return Result<T> { truncated(static_cast<std::uint16_t>(item.payload.size()), item.detail()) };
    }

    return Result<T> { length_mismatch(item.detail(), static_cast<std::uint16_t>(item.payload.size())) };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Real-valued payloads.
// ---------------------------------------------------------------------------

// XDI 0x0810. The sensor's internal temperature.
struct Temperature
{
    static constexpr DataId kId = DataId::Temperature;

    double temperatureC { 0.0 };

    static constexpr Result<Temperature> parse(const DataItem& item)
    {
        const Result<std::array<double, 1>> v = detail::parse_reals<1>(item);
        if (!v) { return std::unexpected(v.error()); }
        return Temperature { (*v)[0] };
    }
};

// XDI 0x4010. The strapdown-integration velocity increment over the sample
// interval. Not an acceleration: it is already multiplied by dt, which is what
// makes it immune to the aliasing a sampled acceleration suffers.
struct DeltaV
{
    static constexpr DataId kId = DataId::DeltaV;

    double xMps { 0.0 };
    double yMps { 0.0 };
    double zMps { 0.0 };

    static constexpr Result<DeltaV> parse(const DataItem& item)
    {
        const Result<std::array<double, 3>> v = detail::parse_reals<3>(item);
        if (!v) { return std::unexpected(v.error()); }
        return DeltaV { (*v)[0], (*v)[1], (*v)[2] };
    }
};

// XDI 0x4020. Calibrated acceleration.
//
// THIS INCLUDES GRAVITY. It is specific force, so a device sitting still on a
// bench reads about 9.81 on whichever axis points up, not zero. The
// gravity-removed output is FreeAcceleration (0x4030), which needs the
// orientation filter and which an MTi-610 therefore cannot produce.
struct Acceleration
{
    static constexpr DataId kId = DataId::Acceleration;

    double xMps2 { 0.0 };
    double yMps2 { 0.0 };
    double zMps2 { 0.0 };

    static constexpr Result<Acceleration> parse(const DataItem& item)
    {
        const Result<std::array<double, 3>> v = detail::parse_reals<3>(item);
        if (!v) { return std::unexpected(v.error()); }
        return Acceleration { (*v)[0], (*v)[1], (*v)[2] };
    }
};

// XDI 0x4040. The high-rate accelerometer tap, ~2000 Hz on a 600-series.
//
// IT IS NOT TIME-ALIGNED WITH ANYTHING ELSE. The LLCP is explicit: on the
// 600-series this output has not been through the strapdown integration and is
// not grouped with the messages coming out at the same instant. Pairing it with
// an Acceleration from the same packet would be pairing two different moments.
struct AccelerationHr
{
    static constexpr DataId kId = DataId::AccelerationHr;

    double xMps2 { 0.0 };
    double yMps2 { 0.0 };
    double zMps2 { 0.0 };

    static constexpr Result<AccelerationHr> parse(const DataItem& item)
    {
        const Result<std::array<double, 3>> v = detail::parse_reals<3>(item);
        if (!v) { return std::unexpected(v.error()); }
        return AccelerationHr { (*v)[0], (*v)[1], (*v)[2] };
    }
};

// XDI 0x8020. Calibrated rate of turn.
struct RateOfTurn
{
    static constexpr DataId kId = DataId::RateOfTurn;

    double xRadps { 0.0 };
    double yRadps { 0.0 };
    double zRadps { 0.0 };

    static constexpr Result<RateOfTurn> parse(const DataItem& item)
    {
        const Result<std::array<double, 3>> v = detail::parse_reals<3>(item);
        if (!v) { return std::unexpected(v.error()); }
        return RateOfTurn { (*v)[0], (*v)[1], (*v)[2] };
    }
};

// XDI 0x8040. The high-rate gyroscope tap, ~1600 Hz on a 600-series. The same
// alignment caveat as AccelerationHr.
struct RateOfTurnHr
{
    static constexpr DataId kId = DataId::RateOfTurnHr;

    double xRadps { 0.0 };
    double yRadps { 0.0 };
    double zRadps { 0.0 };

    static constexpr Result<RateOfTurnHr> parse(const DataItem& item)
    {
        const Result<std::array<double, 3>> v = detail::parse_reals<3>(item);
        if (!v) { return std::unexpected(v.error()); }
        return RateOfTurnHr { (*v)[0], (*v)[1], (*v)[2] };
    }
};

// XDI 0x8030. The orientation increment over the sample interval, as a
// quaternion. Wire order is w, x, y, z -- the LLCP writes it dq0..dq3.
struct DeltaQ
{
    static constexpr DataId kId = DataId::DeltaQ;

    double w { 1.0 };
    double x { 0.0 };
    double y { 0.0 };
    double z { 0.0 };

    static constexpr Result<DeltaQ> parse(const DataItem& item)
    {
        const Result<std::array<double, 4>> v = detail::parse_reals<4>(item);
        if (!v) { return std::unexpected(v.error()); }
        return DeltaQ { (*v)[0], (*v)[1], (*v)[2], (*v)[3] };
    }
};

// XDI 0xC020. The magnetic field.
//
// THE UNIT IS "ARBITRARY UNITS", and that is the LLCP's own wording, not a
// gap in this comment. The device normalises to roughly 1.0 at the local field
// strength during calibration. It is not tesla, not gauss, and not convertible
// to either without knowing the field where the device was calibrated -- which
// is why the field name says Au and the schema's says Au too.
struct MagneticField
{
    static constexpr DataId kId = DataId::MagneticField;

    double xAu { 0.0 };
    double yAu { 0.0 };
    double zAu { 0.0 };

    static constexpr Result<MagneticField> parse(const DataItem& item)
    {
        const Result<std::array<double, 3>> v = detail::parse_reals<3>(item);
        if (!v) { return std::unexpected(v.error()); }
        return MagneticField { (*v)[0], (*v)[1], (*v)[2] };
    }
};

// ---------------------------------------------------------------------------
// Fixed-layout payloads.
// ---------------------------------------------------------------------------

// XDI 0x1010. The device's own UTC clock. On an MTi-610 there is no GNSS
// behind it, so it is only as good as the last SetUtcTime the host sent.
struct UtcTime
{
    static constexpr DataId kId = DataId::UtcTime;
    static constexpr std::size_t kSize = 12;

    std::uint32_t nanosecond { 0 };
    std::uint16_t year { 0 };
    std::uint8_t month { 0 };
    std::uint8_t day { 0 };
    std::uint8_t hour { 0 };
    std::uint8_t minute { 0 };
    std::uint8_t second { 0 };
    std::uint8_t flags { 0 };

    // The LLCP documents three validity bits on the flags byte. They are kept
    // raw as well, because Xsens reserves the rest and firmware adds to them.
    constexpr bool dateValid() const { return bit(flags, 0); }
    constexpr bool timeValid() const { return bit(flags, 1); }
    constexpr bool fullyResolved() const { return bit(flags, 2); }

    static constexpr Result<UtcTime> parse(const DataItem& item)
    {
        if (const auto err = detail::require<UtcTime>(item, kSize)) { return *err; }

        const std::span<const std::uint8_t> b = item.payload;
        return UtcTime {
            .nanosecond = read_u32(b, 0),
            .year = read_u16(b, 4),
            .month = read_u8(b, 6),
            .day = read_u8(b, 7),
            .hour = read_u8(b, 8),
            .minute = read_u8(b, 9),
            .second = read_u8(b, 10),
            .flags = read_u8(b, 11),
        };
    }
};

// XDI 0x1020. Increments once per packet and WRAPS AT 65536, which is about
// eleven minutes at 100 Hz. Anything counting dropped samples has to unwrap it.
struct PacketCounter
{
    static constexpr DataId kId = DataId::PacketCounter;
    static constexpr std::size_t kSize = 2;

    std::uint16_t counter { 0 };

    static constexpr Result<PacketCounter> parse(const DataItem& item)
    {
        if (const auto err = detail::require<PacketCounter>(item, kSize)) { return *err; }
        return PacketCounter { read_u16(item.payload, 0) };
    }
};

// XDI 0x1060. The sample instant in 10 kHz ticks.
//
// WHERE IT WRAPS ON A 600-SERIES IS UNDOCUMENTED. The LLCP gives the 1-series
// 0xFFFFFFFF and the 10/100-series exactly one day (864000000 ticks) and says
// nothing about the 600s. So the ticks are reported raw and unwrapping is left
// to a consumer that knows which assumption it wants to make. See
// docs/nodes/mti610_bridge.md.
struct SampleTimeFine
{
    static constexpr DataId kId = DataId::SampleTimeFine;
    static constexpr std::size_t kSize = 4;

    static constexpr double kTicksPerSecond = 10000.0;

    std::uint32_t ticks { 0 };

    static constexpr Result<SampleTimeFine> parse(const DataItem& item)
    {
        if (const auto err = detail::require<SampleTimeFine>(item, kSize)) { return *err; }
        return SampleTimeFine { read_u32(item.payload, 0) };
    }
};

// XDI 0x1070. The sample instant in whole seconds, to be combined with
// SampleTimeFine's sub-second part.
struct SampleTimeCoarse
{
    static constexpr DataId kId = DataId::SampleTimeCoarse;
    static constexpr std::size_t kSize = 4;

    std::uint32_t seconds { 0 };

    static constexpr Result<SampleTimeCoarse> parse(const DataItem& item)
    {
        if (const auto err = detail::require<SampleTimeCoarse>(item, kSize)) { return *err; }
        return SampleTimeCoarse { read_u32(item.payload, 0) };
    }
};

// XDI 0x3010. Barometric pressure, in whole pascals.
struct BaroPressure
{
    static constexpr DataId kId = DataId::BaroPressure;
    static constexpr std::size_t kSize = 4;

    std::uint32_t pressurePa { 0 };

    static constexpr Result<BaroPressure> parse(const DataItem& item)
    {
        if (const auto err = detail::require<BaroPressure>(item, kSize)) { return *err; }
        return BaroPressure { read_u32(item.payload, 0) };
    }
};

// XDI 0xE010. The low eight bits of the status word, for a device configured
// to send the short form.
struct StatusByte
{
    static constexpr DataId kId = DataId::StatusByte;
    static constexpr std::size_t kSize = 1;

    std::uint8_t status { 0 };

    static constexpr Result<StatusByte> parse(const DataItem& item)
    {
        if (const auto err = detail::require<StatusByte>(item, kSize)) { return *err; }
        return StatusByte { read_u8(item.payload, 0) };
    }
};

// XDI 0xE020. The 32-bit status word, LLCP Table 27.
//
// The word is kept raw AND given named accessors. The raw value is what the
// node publishes alongside the decoded bits, because Xsens reserves bits
// 27..31 and firmware adds to them -- a consumer that only ever sees our
// decode cannot tell a new bit from a clear one.
struct StatusWord
{
    static constexpr DataId kId = DataId::StatusWord;
    static constexpr std::size_t kSize = 4;

    std::uint32_t status { 0 };

    constexpr bool selfTestPassed() const { return bit(status, 0); }
    constexpr bool filterValid() const { return bit(status, 1); }
    // Only meaningful on a GNSS/INS device; always clear on a 610.
    constexpr bool gnssFix() const { return bit(status, 2); }

    // Bits 3:4, the SetNoRotation procedure. 0b11 running, 0b10 rotation
    // detected (sticky), 0b00 complete.
    constexpr std::uint8_t noRotationStatus() const
    {
        return static_cast<std::uint8_t>((status >> 3) & 0x3u);
    }

    constexpr bool representativeMotion() const { return bit(status, 5); }
    constexpr bool clockBiasEstimation() const { return bit(status, 6); }

    constexpr bool clipAccX() const { return bit(status, 8); }
    constexpr bool clipAccY() const { return bit(status, 9); }
    constexpr bool clipAccZ() const { return bit(status, 10); }
    constexpr bool clipGyrX() const { return bit(status, 11); }
    constexpr bool clipGyrY() const { return bit(status, 12); }
    constexpr bool clipGyrZ() const { return bit(status, 13); }
    constexpr bool clipMagX() const { return bit(status, 14); }
    constexpr bool clipMagY() const { return bit(status, 15); }
    constexpr bool clipMagZ() const { return bit(status, 16); }

    // Bit 19. Set when any of bits 8..16 is set, so it is the one bit worth
    // watching -- a sensor driven out of range reports values that look
    // entirely reasonable.
    constexpr bool clipping() const { return bit(status, 19); }

    constexpr bool syncInMarker() const { return bit(status, 21); }
    constexpr bool syncOutMarker() const { return bit(status, 22); }

    // Bits 23:25, GNSS/INS only.
    constexpr std::uint8_t filterMode() const
    {
        return static_cast<std::uint8_t>((status >> 23) & 0x7u);
    }

    constexpr bool haveGnssTimePulse() const { return bit(status, 26); }

    static constexpr Result<StatusWord> parse(const DataItem& item)
    {
        if (const auto err = detail::require<StatusWord>(item, kSize)) { return *err; }
        return StatusWord { read_u32(item.payload, 0) };
    }
};

// Parse one item and hand the result to a generic visitor.
//
// Deliberately not a std::variant: the alternatives range from one byte to
// four doubles, so a variant would be the size of its largest member for every
// item walked. The visitor is called with the concrete type, or with the
// unparsed DataItem when the identifier is not in the table -- which is not an
// error, because an item is self-delimiting and an unmodelled one is skippable.
template <typename Visitor>
constexpr void visit_item(const DataItem& item, Visitor&& visitor)
{
    switch (item.type())
    {
#define XBUS_DATA_VISIT(rawId, Name, snake, elements)   \
    case rawId:                                          \
    {                                                    \
        visitor(Name::parse(item));                      \
        return;                                          \
    }
        XBUS_DATA_TABLE(XBUS_DATA_VISIT)
#undef XBUS_DATA_VISIT
        default: break;
    }

    visitor(item);
}

} // namespace xbus

#endif // XBUS_MTDATA2_H
