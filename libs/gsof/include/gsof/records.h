// SPDX-License-Identifier: GPL-3.0-or-later
//
// One struct per GSOF record, each with a constexpr parse().
//
// Layouts come from the Trimble ICD (receiverhelp.trimble.com/oem-gnss/,
// "GSOF messages"). The ICD numbers its byte offsets from the record TYPE
// byte; parse() is handed the record body only, so every offset here is the
// ICD's minus two. That is the single most likely place to introduce an
// off-by-two, which is why each struct carries its ICD offsets in a comment
// and why tests/test_records.cpp checks them against real captures.
//
// Conventions, all of them load-bearing:
//
//   * FIELD NAMES CARRY UNITS, and the units are the WIRE's, not the ones a
//     consumer wants. Records 2, 27 and 41 are radians; record 49 is degrees.
//     Converting here would mean the struct no longer describes the bytes, and
//     the one place a reader can check a parser against an ICD is the struct.
//     The node converts, once, on the way to capnp.
//
//   * A RECORD LONGER THAN kSize IS ACCEPTED and its tail ignored. Trimble
//     extends records in place -- record 8 grew a local heading, record 27
//     grew a variance block, record 38 has grown twice -- and a parser that
//     required an exact length would turn a firmware update into a total
//     outage for that record. Shorter than kSize is still a hard error.
//
//   * Flag bytes are kept raw AND given named accessors. The raw byte is what
//     gets published (so a bit we have not named is not lost); the accessors
//     are what the code reads.

#ifndef GSOF_RECORDS_H
#define GSOF_RECORDS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "gsof/byte_order.h"
#include "gsof/error.h"
#include "gsof/record_table.h"

namespace gsof
{

// The wire byte for each record we model. Generated from the table so it
// cannot drift from the parsers.
enum class RecordType : std::uint8_t
{
#define GSOF_RECORD_ENUM(id, Name, snake) Name = id,
    GSOF_RECORD_TABLE(GSOF_RECORD_ENUM)
#undef GSOF_RECORD_ENUM
};

constexpr const char* record_name(RecordType type)
{
    switch (type)
    {
#define GSOF_RECORD_NAME(id, Name, snake) case RecordType::Name: return snake;
        GSOF_RECORD_TABLE(GSOF_RECORD_NAME)
#undef GSOF_RECORD_NAME
    }

    // Reached for any record type not in the table. Not a default:, so adding
    // a row without a name is a compile error rather than a silent "unknown".
    return "unknown";
}

// True when `type` is one of the records we model.
constexpr bool is_known_record(std::uint8_t type)
{
    switch (static_cast<RecordType>(type))
    {
#define GSOF_RECORD_KNOWN(id, Name, snake) case RecordType::Name: return true;
        GSOF_RECORD_TABLE(GSOF_RECORD_KNOWN)
#undef GSOF_RECORD_KNOWN
    }

    return false;
}

namespace detail
{

// The length check every parser starts with. Short is fatal, long is fine --
// see the header comment on forward compatibility. Returns the failure ready to
// propagate, or nullopt when the buffer is long enough.
//
// Templated on the caller's record, rather than the plainer Result<void>, so the
// failure is built as the caller's own Result<T> and returned as-is. The obvious
// spelling -- return a Result<void>, then `return std::unexpected(ok.error())` --
// makes each parser copy an Error out of one expected and into the union of
// another, and that union is as wide as the record while Error is four bytes.
// GCC 15 duly widens the copy to the whole union and warns that the bytes past
// Error's end are uninitialised. They are, and nothing reads them, but the round
// trip through the stack slot is real work on a path that has none to do.
template <typename T>
constexpr std::optional<Result<T>> require(std::span<const std::uint8_t> bytes, std::size_t need, RecordType type)
{
    if (bytes.size() >= need)
    {
        return std::nullopt;
    }
    return Result<T> { truncated(static_cast<std::uint16_t>(bytes.size()), static_cast<std::uint8_t>(type)) };
}

} // namespace detail

// ============================================================================
// Time and position
// ============================================================================

// Record 1. ICD bytes 2..11.
//
// This is the ONLY record that puts time-of-week before the week number.
// Everything else -- 16, 41, 49, 50 -- is week first. Getting it the other way
// round yields a week number in the millions and a time of 8-something, both
// of which are obviously wrong, so this one fails loudly rather than quietly.
struct PositionTime
{
    static constexpr RecordType kType = RecordType::PositionTime;
    static constexpr std::size_t kSize = 10;

    std::uint32_t gpsTimeMs { 0 };   // ICD 2..5,  ms into the GPS week
    std::uint16_t gpsWeek { 0 };     // ICD 6..7
    std::uint8_t svsUsed { 0 };      // ICD 8
    std::uint8_t positionFlags1 { 0 };  // ICD 9
    std::uint8_t positionFlags2 { 0 };  // ICD 10
    std::uint8_t initCounter { 0 };     // ICD 11, increments on each RTK init

    // positionFlags1
    constexpr bool isNewPosition() const { return bit(positionFlags1, 0); }
    constexpr bool hasClockFix() const { return bit(positionFlags1, 1); }
    constexpr bool horizontalComputedHere() const { return bit(positionFlags1, 2); }
    constexpr bool heightComputedHere() const { return bit(positionFlags1, 3); }
    constexpr bool isWeightedLeastSquares() const { return bit(positionFlags1, 4); }
    constexpr bool usedL1Pseudorange() const { return bit(positionFlags1, 5); }
    constexpr bool isDifferential() const { return bit(positionFlags1, 6); }
    constexpr bool isDifferentialPhase() const { return bit(positionFlags1, 7); }

    // positionFlags2
    constexpr bool isFixedInteger() const { return bit(positionFlags2, 0); }
    constexpr bool isOmniStar() const { return bit(positionFlags2, 1); }
    constexpr bool isStaticConstrained() const { return bit(positionFlags2, 2); }
    constexpr bool isNetworkRtk() const { return bit(positionFlags2, 3); }
    constexpr bool isLocationRtk() const { return bit(positionFlags2, 4); }
    constexpr bool isBeaconDgps() const { return bit(positionFlags2, 5); }

    static constexpr Result<PositionTime> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<PositionTime>(b, kSize, kType))
        {
            return *err;
        }

        return PositionTime {
            .gpsTimeMs = read_u32(b, 0),
            .gpsWeek = read_u16(b, 4),
            .svsUsed = read_u8(b, 6),
            .positionFlags1 = read_u8(b, 7),
            .positionFlags2 = read_u8(b, 8),
            .initCounter = read_u8(b, 9),
        };
    }
};

// Record 2. ICD bytes 2..25. WGS-84, radians.
struct LatLongHeight
{
    static constexpr RecordType kType = RecordType::LatLongHeight;
    static constexpr std::size_t kSize = 24;

    double latitudeRad { 0.0 };
    double longitudeRad { 0.0 };
    double heightM { 0.0 };   // above the WGS-84 ellipsoid, not above sea level

    static constexpr Result<LatLongHeight> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<LatLongHeight>(b, kSize, kType))
        {
            return *err;
        }

        return LatLongHeight { read_f64(b, 0), read_f64(b, 8), read_f64(b, 16) };
    }
};

// Record 3. ICD bytes 2..25.
struct EcefPosition
{
    static constexpr RecordType kType = RecordType::EcefPosition;
    static constexpr std::size_t kSize = 24;

    double xM { 0.0 };
    double yM { 0.0 };
    double zM { 0.0 };

    static constexpr Result<EcefPosition> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<EcefPosition>(b, kSize, kType))
        {
            return *err;
        }

        return EcefPosition { read_f64(b, 0), read_f64(b, 8), read_f64(b, 16) };
    }
};

// Record 6. ICD bytes 2..25. Rover minus base, in ECEF.
struct EcefDelta
{
    static constexpr RecordType kType = RecordType::EcefDelta;
    static constexpr std::size_t kSize = 24;

    double deltaXM { 0.0 };
    double deltaYM { 0.0 };
    double deltaZM { 0.0 };

    static constexpr Result<EcefDelta> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<EcefDelta>(b, kSize, kType))
        {
            return *err;
        }

        return EcefDelta { read_f64(b, 0), read_f64(b, 8), read_f64(b, 16) };
    }
};

// Record 7. ICD bytes 2..25. The same baseline as record 6, rotated into the
// base's local tangent plane.
struct TangentPlaneDelta
{
    static constexpr RecordType kType = RecordType::TangentPlaneDelta;
    static constexpr std::size_t kSize = 24;

    double eastM { 0.0 };
    double northM { 0.0 };
    double upM { 0.0 };

    static constexpr Result<TangentPlaneDelta> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<TangentPlaneDelta>(b, kSize, kType))
        {
            return *err;
        }

        return TangentPlaneDelta { read_f64(b, 0), read_f64(b, 8), read_f64(b, 16) };
    }
};

// Record 8. ICD bytes 2..14, or 2..18 with the local heading.
//
// The two lengths are the only thing that distinguishes them: there is no flag
// saying the trailing field is present.
struct Velocity
{
    static constexpr RecordType kType = RecordType::Velocity;
    static constexpr std::size_t kSize = 13;
    static constexpr std::size_t kSizeWithLocalHeading = 17;

    std::uint8_t velocityFlags { 0 };
    float horizontalSpeedMps { 0.0F };
    float headingRad { 0.0F };        // true north
    float verticalVelocityMps { 0.0F };

    bool hasLocalHeading { false };
    float localHeadingRad { 0.0F };   // relative to the local coordinate system

    constexpr bool isValid() const { return bit(velocityFlags, 0); }
    // Clear means the velocity was differenced from consecutive positions,
    // which is noisier and lags.
    constexpr bool isDopplerDerived() const { return bit(velocityFlags, 1); }

    static constexpr Result<Velocity> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<Velocity>(b, kSize, kType))
        {
            return *err;
        }

        Velocity out {};
        out.velocityFlags = read_u8(b, 0);
        out.horizontalSpeedMps = read_f32(b, 1);
        out.headingRad = read_f32(b, 5);
        out.verticalVelocityMps = read_f32(b, 9);

        if (b.size() >= kSizeWithLocalHeading)
        {
            out.hasLocalHeading = true;
            out.localHeadingRad = read_f32(b, 13);
        }

        return out;
    }
};

// Record 9. ICD bytes 2..17.
struct DopInfo
{
    static constexpr RecordType kType = RecordType::DopInfo;
    static constexpr std::size_t kSize = 16;

    float pdop { 0.0F };
    float hdop { 0.0F };
    float vdop { 0.0F };
    float tdop { 0.0F };

    static constexpr Result<DopInfo> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<DopInfo>(b, kSize, kType))
        {
            return *err;
        }

        return DopInfo { read_f32(b, 0), read_f32(b, 4), read_f32(b, 8), read_f32(b, 12) };
    }
};

// Record 10. ICD bytes 2..18.
struct ClockInfo
{
    static constexpr RecordType kType = RecordType::ClockInfo;
    static constexpr std::size_t kSize = 17;

    std::uint8_t clockFlags { 0 };
    double receiverClockOffsetMs { 0.0 };
    double frequencyOffsetPpm { 0.0 };

    constexpr bool isClockOffsetValid() const { return bit(clockFlags, 0); }
    constexpr bool isFrequencyOffsetValid() const { return bit(clockFlags, 1); }
    constexpr bool isReceiverInAnywhereFix() const { return bit(clockFlags, 2); }

    static constexpr Result<ClockInfo> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<ClockInfo>(b, kSize, kType))
        {
            return *err;
        }

        return ClockInfo { read_u8(b, 0), read_f64(b, 1), read_f64(b, 9) };
    }
};

// Record 11. ICD bytes 2..35. The ECEF variance-covariance matrix, upper
// triangle only -- it is symmetric, so xy == yx.
struct PositionVcv
{
    static constexpr RecordType kType = RecordType::PositionVcv;
    static constexpr std::size_t kSize = 34;

    float positionRms { 0.0F };
    float xx { 0.0F };
    float xy { 0.0F };
    float xz { 0.0F };
    float yy { 0.0F };
    float yz { 0.0F };
    float zz { 0.0F };
    float unitVariance { 0.0F };
    std::uint16_t epochCount { 0 };

    static constexpr Result<PositionVcv> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<PositionVcv>(b, kSize, kType))
        {
            return *err;
        }

        return PositionVcv {
            read_f32(b, 0),  read_f32(b, 4),  read_f32(b, 8),  read_f32(b, 12),
            read_f32(b, 16), read_f32(b, 20), read_f32(b, 24), read_f32(b, 28),
            read_u16(b, 32),
        };
    }
};

// Record 12. ICD bytes 2..39. The same uncertainty as record 11, expressed in
// the local horizontal plane, which is the form a human reads.
struct PositionSigma
{
    static constexpr RecordType kType = RecordType::PositionSigma;
    static constexpr std::size_t kSize = 38;

    float positionRms { 0.0F };
    float sigmaEastM { 0.0F };
    float sigmaNorthM { 0.0F };
    float covarianceEastNorth { 0.0F };
    float sigmaUpM { 0.0F };
    float semiMajorM { 0.0F };
    float semiMinorM { 0.0F };
    float orientationDeg { 0.0F };   // of the error ellipse, from true north
    float unitVariance { 0.0F };
    std::uint16_t epochCount { 0 };

    static constexpr Result<PositionSigma> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<PositionSigma>(b, kSize, kType))
        {
            return *err;
        }

        return PositionSigma {
            read_f32(b, 0),  read_f32(b, 4),  read_f32(b, 8),  read_f32(b, 12),
            read_f32(b, 16), read_f32(b, 20), read_f32(b, 24), read_f32(b, 28),
            read_f32(b, 32), read_u16(b, 36),
        };
    }
};

// Record 15. ICD bytes 2..5.
struct ReceiverSerial
{
    static constexpr RecordType kType = RecordType::ReceiverSerial;
    static constexpr std::size_t kSize = 4;

    std::int32_t serialNumber { 0 };

    static constexpr Result<ReceiverSerial> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<ReceiverSerial>(b, kSize, kType))
        {
            return *err;
        }

        return ReceiverSerial { read_i32(b, 0) };
    }
};

// Record 16. ICD bytes 2..10.
//
// utcOffsetS is GPS minus UTC in whole seconds -- 18 since 2017. It is signed
// in the ICD even though it has only ever been positive.
struct CurrentTimeUtc
{
    static constexpr RecordType kType = RecordType::CurrentTimeUtc;
    static constexpr std::size_t kSize = 9;

    std::uint32_t gpsTimeMs { 0 };
    std::uint16_t gpsWeek { 0 };
    std::int16_t utcOffsetS { 0 };
    std::uint8_t timeFlags { 0 };

    constexpr bool isTimeValid() const { return bit(timeFlags, 0); }
    constexpr bool isUtcOffsetValid() const { return bit(timeFlags, 1); }

    static constexpr Result<CurrentTimeUtc> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<CurrentTimeUtc>(b, kSize, kType))
        {
            return *err;
        }

        return CurrentTimeUtc { read_u32(b, 0), read_u16(b, 4), read_i16(b, 6), read_u8(b, 8) };
    }
};

// ============================================================================
// Attitude -- the reason a BD992 has two antennas
// ============================================================================

// Record 27. ICD bytes 2..43, or 2..71 with the variance block.
//
// The variance block arrived in GNSS firmware 4.20 and, exactly as with record
// 8, there is no flag for it: presence is inferred from the record length.
//
// Pitch, yaw, roll -- in that order on the wire, which is not the order the
// field is usually spoken in. On a two-antenna receiver only yaw (heading) and
// pitch are observable; roll comes out of the solver but is not a measurement.
struct AttitudeInfo
{
    static constexpr RecordType kType = RecordType::AttitudeInfo;
    static constexpr std::size_t kSize = 42;
    static constexpr std::size_t kSizeWithVariance = 70;

    std::uint32_t gpsTimeMs { 0 };
    std::uint8_t attitudeFlags { 0 };
    std::uint8_t svsUsed { 0 };
    std::uint8_t calculationMode { 0 };

    double pitchRad { 0.0 };
    double yawRad { 0.0 };     // heading, relative to true north
    double rollRad { 0.0 };
    double masterSlaveRangeM { 0.0 };   // the antenna baseline length
    std::uint16_t pdopScaled { 0 };     // units of 0.1

    bool hasVariance { false };
    float pitchVariance { 0.0F };
    float yawVariance { 0.0F };
    float rollVariance { 0.0F };
    float pitchYawCovariance { 0.0F };
    float pitchRollCovariance { 0.0F };
    float yawRollCovariance { 0.0F };
    float masterSlaveRangeVariance { 0.0F };

    constexpr bool isCalibrated() const { return bit(attitudeFlags, 0); }
    constexpr bool isPitchValid() const { return bit(attitudeFlags, 1); }
    constexpr bool isYawValid() const { return bit(attitudeFlags, 2); }
    constexpr bool isRollValid() const { return bit(attitudeFlags, 3); }

    constexpr double pdop() const { return static_cast<double>(pdopScaled) * 0.1; }

    static constexpr Result<AttitudeInfo> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<AttitudeInfo>(b, kSize, kType))
        {
            return *err;
        }

        AttitudeInfo out {};
        out.gpsTimeMs = read_u32(b, 0);
        out.attitudeFlags = read_u8(b, 4);
        out.svsUsed = read_u8(b, 5);
        out.calculationMode = read_u8(b, 6);
        // b[7] is reserved.
        out.pitchRad = read_f64(b, 8);
        out.yawRad = read_f64(b, 16);
        out.rollRad = read_f64(b, 24);
        out.masterSlaveRangeM = read_f64(b, 32);
        out.pdopScaled = read_u16(b, 40);

        if (b.size() >= kSizeWithVariance)
        {
            out.hasVariance = true;
            out.pitchVariance = read_f32(b, 42);
            out.yawVariance = read_f32(b, 46);
            out.rollVariance = read_f32(b, 50);
            out.pitchYawCovariance = read_f32(b, 54);
            out.pitchRollCovariance = read_f32(b, 58);
            out.yawRollCovariance = read_f32(b, 62);
            out.masterSlaveRangeVariance = read_f32(b, 66);
        }

        return out;
    }
};

// ============================================================================
// Satellites
// ============================================================================

enum class SvSystem : std::uint8_t
{
    Gps = 0,
    Sbas = 1,
    Glonass = 2,
    Galileo = 3,
    Qzss = 4,
    BeiDou = 5,
};

constexpr const char* to_string(SvSystem system)
{
    switch (system)
    {
        case SvSystem::Gps:     return "GPS";
        case SvSystem::Sbas:    return "SBAS";
        case SvSystem::Glonass: return "GLONASS";
        case SvSystem::Galileo: return "Galileo";
        case SvSystem::Qzss:    return "QZSS";
        case SvSystem::BeiDou:  return "BeiDou";
    }

    // 6..255 are reserved in the ICD, and a receiver newer than this build will
    // use them.
    return "unknown";
}

// Flags shared by records 33 and 34.
struct SvFlags
{
    std::uint8_t flags1 { 0 };
    std::uint8_t flags2 { 0 };

    constexpr bool aboveHorizon() const { return bit(flags1, 0); }
    constexpr bool assignedToChannel() const { return bit(flags1, 1); }
    constexpr bool trackedFirstFreq() const { return bit(flags1, 2); }
    constexpr bool trackedSecondFreq() const { return bit(flags1, 3); }
    constexpr bool baseReportedFirstFreq() const { return bit(flags1, 4); }
    constexpr bool baseReportedSecondFreq() const { return bit(flags1, 5); }
    constexpr bool usedInPosition() const { return bit(flags1, 6); }
    constexpr bool usedInRtk() const { return bit(flags1, 7); }
};

struct SvBrief
{
    std::uint8_t prn { 0 };
    std::uint8_t system { 0 };
    SvFlags flags {};
};

struct SvDetail
{
    std::uint8_t prn { 0 };
    std::uint8_t system { 0 };
    SvFlags flags {};
    std::uint8_t elevationDeg { 0 };
    std::uint16_t azimuthDeg { 0 };
    // The ICD's units are dB times four. Kept scaled so the struct still
    // describes the bytes; snr*Db() does the division.
    std::uint8_t snrFirstScaled { 0 };
    std::uint8_t snrSecondScaled { 0 };
    std::uint8_t snrThirdScaled { 0 };

    constexpr float snrFirstDb() const { return static_cast<float>(snrFirstScaled) * 0.25F; }
    constexpr float snrSecondDb() const { return static_cast<float>(snrSecondScaled) * 0.25F; }
    constexpr float snrThirdDb() const { return static_cast<float>(snrThirdScaled) * 0.25F; }
};

// A record's length byte is one byte wide, so these are hard ceilings rather
// than chosen limits: (255 - 1) / 4 and (255 - 1) / 10. Storing the satellites
// inline keeps the record a literal type, which is what lets a golden capture
// be checked by static_assert.
inline constexpr std::size_t kMaxSvBrief = 63;
inline constexpr std::size_t kMaxSvDetailed = 25;

// Record 33. ICD byte 2 is the count, then 4 bytes per satellite.
struct AllSvBrief
{
    static constexpr RecordType kType = RecordType::AllSvBrief;
    static constexpr std::size_t kSize = 1;
    static constexpr std::size_t kEntrySize = 4;

    std::uint8_t count { 0 };
    std::array<SvBrief, kMaxSvBrief> satellites {};

    constexpr std::span<const SvBrief> view() const
    {
        return std::span<const SvBrief>(satellites.data(), count);
    }

    static constexpr Result<AllSvBrief> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<AllSvBrief>(b, kSize, kType))
        {
            return *err;
        }

        AllSvBrief out {};
        out.count = read_u8(b, 0);

        if (out.count > kMaxSvBrief)
        {
            return length_mismatch(static_cast<std::uint8_t>(kType), out.count);
        }

        // The count and the record length must agree. They are two independent
        // statements of the same fact, so a disagreement means one of them is
        // corrupt and neither can be trusted.
        const std::size_t need = kSize + static_cast<std::size_t>(out.count) * kEntrySize;
        if (b.size() < need)
        {
            return length_mismatch(static_cast<std::uint8_t>(kType), static_cast<std::uint16_t>(b.size()));
        }

        for (std::size_t i = 0; i < out.count; ++i)
        {
            const std::size_t at = kSize + i * kEntrySize;
            out.satellites[i] = SvBrief {
                read_u8(b, at),
                read_u8(b, at + 1),
                SvFlags { read_u8(b, at + 2), read_u8(b, at + 3) },
            };
        }

        return out;
    }
};

// Record 34. ICD byte 2 is the count, then 10 bytes per satellite.
struct AllSvDetailed
{
    static constexpr RecordType kType = RecordType::AllSvDetailed;
    static constexpr std::size_t kSize = 1;
    static constexpr std::size_t kEntrySize = 10;

    std::uint8_t count { 0 };
    std::array<SvDetail, kMaxSvDetailed> satellites {};

    constexpr std::span<const SvDetail> view() const
    {
        return std::span<const SvDetail>(satellites.data(), count);
    }

    static constexpr Result<AllSvDetailed> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<AllSvDetailed>(b, kSize, kType))
        {
            return *err;
        }

        AllSvDetailed out {};
        out.count = read_u8(b, 0);

        if (out.count > kMaxSvDetailed)
        {
            return length_mismatch(static_cast<std::uint8_t>(kType), out.count);
        }

        const std::size_t need = kSize + static_cast<std::size_t>(out.count) * kEntrySize;
        if (b.size() < need)
        {
            return length_mismatch(static_cast<std::uint8_t>(kType), static_cast<std::uint16_t>(b.size()));
        }

        for (std::size_t i = 0; i < out.count; ++i)
        {
            const std::size_t at = kSize + i * kEntrySize;
            out.satellites[i] = SvDetail {
                .prn = read_u8(b, at),
                .system = read_u8(b, at + 1),
                .flags = SvFlags { read_u8(b, at + 2), read_u8(b, at + 3) },
                .elevationDeg = read_u8(b, at + 4),
                .azimuthDeg = read_u16(b, at + 5),
                .snrFirstScaled = read_u8(b, at + 7),
                .snrSecondScaled = read_u8(b, at + 8),
                .snrThirdScaled = read_u8(b, at + 9),
            };
        }

        return out;
    }
};

// ============================================================================
// Corrections and receiver status
// ============================================================================

// Record 35. ICD bytes 2..36.
struct ReceivedBase
{
    static constexpr RecordType kType = RecordType::ReceivedBase;
    static constexpr std::size_t kSize = 35;
    static constexpr std::size_t kNameLength = 8;

    std::uint8_t baseFlags { 0 };
    // Space padded, not NUL terminated. Kept as fixed bytes rather than
    // trimmed, because trimming is a display decision and the trailing bytes
    // are occasionally not spaces.
    std::array<char, kNameLength> baseName {};
    std::uint16_t baseId { 0 };
    double latitudeRad { 0.0 };
    double longitudeRad { 0.0 };
    double heightM { 0.0 };

    constexpr bool isBaseInfoValid() const { return bit(baseFlags, 3); }

    static constexpr Result<ReceivedBase> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<ReceivedBase>(b, kSize, kType))
        {
            return *err;
        }

        ReceivedBase out {};
        out.baseFlags = read_u8(b, 0);
        for (std::size_t i = 0; i < kNameLength; ++i)
        {
            out.baseName[i] = static_cast<char>(read_u8(b, 1 + i));
        }
        out.baseId = read_u16(b, 9);
        out.latitudeRad = read_f64(b, 11);
        out.longitudeRad = read_f64(b, 19);
        out.heightM = read_f64(b, 27);

        return out;
    }
};

// Record 37. ICD bytes 2..11.
struct BatteryMemory
{
    static constexpr RecordType kType = RecordType::BatteryMemory;
    static constexpr std::size_t kSize = 10;

    std::uint16_t batteryPercent { 0 };
    double remainingMemoryHours { 0.0 };

    static constexpr Result<BatteryMemory> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<BatteryMemory>(b, kSize, kType))
        {
            return *err;
        }

        return BatteryMemory { read_u16(b, 0), read_f64(b, 2) };
    }
};

// The values of PositionType::positionFixType. Not exhaustive of the ICD's
// list -- it runs to 48 with gaps -- but the ones a BD992 in a vehicle can
// produce. The raw byte is always published alongside, so a value not named
// here is reported rather than lost.
enum class PositionFixType : std::uint8_t
{
    NoFixOrOld = 0,
    Autonomous = 1,
    PropagatedAutonomous = 2,
    DifferentialSbas = 3,
    PropagatedSbas = 4,
    Differential = 5,
    PropagatedDifferential = 6,
    FloatRtk = 7,
    PropagatedFloatRtk = 8,
    FixedRtk = 9,
    PropagatedFixedRtk = 10,
    OmniStarHp = 11,
    OmniStarXp = 12,
    LocationRtk = 13,
    OmniStarVbs = 14,
    BeaconDifferential = 15,
    RtxCodePhase = 31,
    XFillRtx = 36,
};

// Record 38. ICD bytes 2..27, and growing.
//
// This record has been extended twice already, which is why the tail is kept
// rather than rejected: a firmware that adds a field must not take the record
// out of service.
struct PositionType
{
    static constexpr RecordType kType = RecordType::PositionType;
    static constexpr std::size_t kSize = 26;

    float errorScale { 0.0F };
    std::uint8_t solutionFlags { 0 };
    std::uint8_t rtkCondition { 0 };
    float correctionAgeS { 0.0F };
    std::uint8_t networkFlags { 0 };
    std::uint8_t networkFlags2 { 0 };
    std::uint8_t frameFlag { 0 };
    std::int16_t itrfEpochCentiYears { 0 };   // since 2005-01-01
    std::uint8_t tectonicPlate { 0 };
    std::int32_t rtxSubscriptionMinutesLeft { 0 };
    std::uint8_t poleWobbleStatus { 0 };
    float poleWobbleDistanceM { 0.0F };
    std::uint8_t positionFixTypeRaw { 0 };

    constexpr bool isNetworkSolution() const { return bit(solutionFlags, 0); }
    // Clear means float, and the difference between an RTK fix and an RTK
    // float is two orders of magnitude of accuracy.
    constexpr bool isRtkFixed() const { return bit(solutionFlags, 1); }
    constexpr std::uint8_t initialisationIntegrity() const
    {
        return static_cast<std::uint8_t>((solutionFlags >> 2) & 0x03u);
    }

    constexpr PositionFixType positionFixType() const
    {
        return static_cast<PositionFixType>(positionFixTypeRaw);
    }

    static constexpr Result<PositionType> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<PositionType>(b, kSize, kType))
        {
            return *err;
        }

        return PositionType {
            .errorScale = read_f32(b, 0),
            .solutionFlags = read_u8(b, 4),
            .rtkCondition = read_u8(b, 5),
            .correctionAgeS = read_f32(b, 6),
            .networkFlags = read_u8(b, 10),
            .networkFlags2 = read_u8(b, 11),
            .frameFlag = read_u8(b, 12),
            .itrfEpochCentiYears = read_i16(b, 13),
            .tectonicPlate = read_u8(b, 15),
            .rtxSubscriptionMinutesLeft = read_i32(b, 16),
            .poleWobbleStatus = read_u8(b, 20),
            .poleWobbleDistanceM = read_f32(b, 21),
            .positionFixTypeRaw = read_u8(b, 25),
        };
    }
};

// Record 40. ICD bytes 2..71.
struct LbandStatus
{
    static constexpr RecordType kType = RecordType::LbandStatus;
    static constexpr std::size_t kSize = 70;
    static constexpr std::size_t kNameLength = 5;

    std::array<char, kNameLength> satelliteName {};
    float nominalFrequencyMhz { 0.0F };
    std::uint16_t satelliteBitRate { 0 };
    float cnoDb { 0.0F };
    std::uint8_t hpxpSubscribedEngine { 0 };
    std::uint8_t hpxpLibraryMode { 0 };
    std::uint8_t vbsLibraryMode { 0 };
    std::uint8_t beamMode { 0 };
    std::uint8_t omniStarMotion { 0 };
    float horizontalSigmaThresholdM { 0.0F };
    float verticalSigmaThresholdM { 0.0F };
    std::uint8_t nmeaEncryptionState { 0 };
    float iqRatio { 0.0F };
    float estimatedBitErrorRate { 0.0F };
    std::uint32_t totalMessages { 0 };
    std::uint32_t totalUniqueWordsWithErrors { 0 };
    std::uint32_t totalBadUniqueWordBits { 0 };
    std::uint32_t totalViterbiSymbols { 0 };
    std::uint32_t correctedViterbiSymbols { 0 };
    std::uint32_t badMessages { 0 };
    std::uint8_t measuredFrequencyValid { 0 };
    double measuredFrequencyHz { 0.0 };

    static constexpr Result<LbandStatus> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<LbandStatus>(b, kSize, kType))
        {
            return *err;
        }

        LbandStatus out {};
        for (std::size_t i = 0; i < kNameLength; ++i)
        {
            out.satelliteName[i] = static_cast<char>(read_u8(b, i));
        }
        out.nominalFrequencyMhz = read_f32(b, 5);
        out.satelliteBitRate = read_u16(b, 9);
        out.cnoDb = read_f32(b, 11);
        out.hpxpSubscribedEngine = read_u8(b, 15);
        out.hpxpLibraryMode = read_u8(b, 16);
        out.vbsLibraryMode = read_u8(b, 17);
        out.beamMode = read_u8(b, 18);
        out.omniStarMotion = read_u8(b, 19);
        out.horizontalSigmaThresholdM = read_f32(b, 20);
        out.verticalSigmaThresholdM = read_f32(b, 24);
        out.nmeaEncryptionState = read_u8(b, 28);
        out.iqRatio = read_f32(b, 29);
        out.estimatedBitErrorRate = read_f32(b, 33);
        out.totalMessages = read_u32(b, 37);
        out.totalUniqueWordsWithErrors = read_u32(b, 41);
        out.totalBadUniqueWordBits = read_u32(b, 45);
        out.totalViterbiSymbols = read_u32(b, 49);
        out.correctedViterbiSymbols = read_u32(b, 53);
        out.badMessages = read_u32(b, 57);
        out.measuredFrequencyValid = read_u8(b, 61);
        out.measuredFrequencyHz = read_f64(b, 62);

        return out;
    }
};

// Record 41. ICD bytes 2..32. Week first, unlike record 1.
struct BasePosition
{
    static constexpr RecordType kType = RecordType::BasePosition;
    static constexpr std::size_t kSize = 31;

    std::uint32_t gpsTimeMs { 0 };
    std::uint16_t gpsWeek { 0 };
    double latitudeRad { 0.0 };
    double longitudeRad { 0.0 };
    double heightM { 0.0 };
    std::uint8_t baseQuality { 0 };

    static constexpr Result<BasePosition> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<BasePosition>(b, kSize, kType))
        {
            return *err;
        }

        return BasePosition {
            read_u32(b, 0), read_u16(b, 4), read_f64(b, 6), read_f64(b, 14), read_f64(b, 22), read_u8(b, 30),
        };
    }
};

// ============================================================================
// INS -- not produced by a BD992, but the captures exist and the rows are cheap
// ============================================================================

// Record 49. ICD bytes 2..105.
//
// NOTE THE UNITS: this record is DEGREES where records 2 and 41 are radians,
// and metres per second where record 8 is too. Trimble's own doing.
struct InsFullNav
{
    static constexpr RecordType kType = RecordType::InsFullNav;
    static constexpr std::size_t kSize = 104;

    std::uint16_t gpsWeek { 0 };
    std::uint32_t gpsTimeMs { 0 };
    std::uint8_t imuAlignmentStatus { 0 };
    std::uint8_t gnssStatus { 0 };

    double latitudeDeg { 0.0 };
    double longitudeDeg { 0.0 };
    double altitudeM { 0.0 };

    float velocityNorthMps { 0.0F };
    float velocityEastMps { 0.0F };
    float velocityDownMps { 0.0F };
    float totalSpeedMps { 0.0F };

    double rollDeg { 0.0 };
    double pitchDeg { 0.0 };
    double headingDeg { 0.0 };
    double trackAngleDeg { 0.0 };

    float angularRateRollDps { 0.0F };
    float angularRatePitchDps { 0.0F };
    float angularRateHeadingDps { 0.0F };

    float accelerationXMps2 { 0.0F };
    float accelerationYMps2 { 0.0F };
    float accelerationZMps2 { 0.0F };

    static constexpr Result<InsFullNav> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<InsFullNav>(b, kSize, kType))
        {
            return *err;
        }

        InsFullNav out {};
        out.gpsWeek = read_u16(b, 0);
        out.gpsTimeMs = read_u32(b, 2);
        out.imuAlignmentStatus = read_u8(b, 6);
        out.gnssStatus = read_u8(b, 7);
        out.latitudeDeg = read_f64(b, 8);
        out.longitudeDeg = read_f64(b, 16);
        out.altitudeM = read_f64(b, 24);
        out.velocityNorthMps = read_f32(b, 32);
        out.velocityEastMps = read_f32(b, 36);
        out.velocityDownMps = read_f32(b, 40);
        out.totalSpeedMps = read_f32(b, 44);
        out.rollDeg = read_f64(b, 48);
        out.pitchDeg = read_f64(b, 56);
        out.headingDeg = read_f64(b, 64);
        out.trackAngleDeg = read_f64(b, 72);
        out.angularRateRollDps = read_f32(b, 80);
        out.angularRatePitchDps = read_f32(b, 84);
        out.angularRateHeadingDps = read_f32(b, 88);
        out.accelerationXMps2 = read_f32(b, 92);
        out.accelerationYMps2 = read_f32(b, 96);
        out.accelerationZMps2 = read_f32(b, 100);

        return out;
    }
};

// Record 50. ICD bytes 2..45.
struct InsRms
{
    static constexpr RecordType kType = RecordType::InsRms;
    static constexpr std::size_t kSize = 44;

    std::uint16_t gpsWeek { 0 };
    std::uint32_t gpsTimeMs { 0 };
    std::uint8_t imuAlignmentStatus { 0 };
    std::uint8_t gnssStatus { 0 };

    float positionRmsNorthM { 0.0F };
    float positionRmsEastM { 0.0F };
    float positionRmsDownM { 0.0F };

    float velocityRmsNorthMps { 0.0F };
    float velocityRmsEastMps { 0.0F };
    float velocityRmsDownMps { 0.0F };

    float rollRmsDeg { 0.0F };
    float pitchRmsDeg { 0.0F };
    float headingRmsDeg { 0.0F };

    static constexpr Result<InsRms> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<InsRms>(b, kSize, kType))
        {
            return *err;
        }

        return InsRms {
            read_u16(b, 0),  read_u32(b, 2),  read_u8(b, 6),   read_u8(b, 7),
            read_f32(b, 8),  read_f32(b, 12), read_f32(b, 16),
            read_f32(b, 20), read_f32(b, 24), read_f32(b, 28),
            read_f32(b, 32), read_f32(b, 36), read_f32(b, 40),
        };
    }
};

} // namespace gsof

#endif // GSOF_RECORDS_H
