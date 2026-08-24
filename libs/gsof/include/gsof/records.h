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

// Record 62. ICD bytes 2..44.
//
// The CODE-ONLY position: the pseudorange solution, before any carrier phase.
// On a receiver holding an RTK fix this differs from record 2 by decimetres and
// is the thing to compare against when a fix is suspect. On the bench receiver,
// which was on an SBAS differential fix, records 2, 12 and 62 agreed bit for
// bit -- which is exactly right, because with no carrier-phase solution the
// code position IS the position.
//
// It is the only position record that carries its own time AND its own sigmas,
// so it is self-contained in a way records 2 and 12 are not.
struct CodePosition
{
    static constexpr RecordType kType = RecordType::CodePosition;
    static constexpr std::size_t kSize = 43;

    // The ICD documents 0..3 and does not name them, so the byte is passed
    // through rather than turned into an enum this tree would be inventing.
    std::uint8_t positionType { 0 };
    double latitudeRad { 0.0 };
    double longitudeRad { 0.0 };
    double heightM { 0.0 };    // above the WGS-84 ellipsoid
    std::uint16_t gpsWeek { 0 };
    std::uint32_t gpsTimeMs { 0 };
    float sigmaEastM { 0.0F };
    float sigmaNorthM { 0.0F };
    float sigmaUpM { 0.0F };

    static constexpr Result<CodePosition> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<CodePosition>(b, kSize, kType))
        {
            return *err;
        }

        return CodePosition {
            .positionType = read_u8(b, 0),
            .latitudeRad = read_f64(b, 1),
            .longitudeRad = read_f64(b, 9),
            .heightM = read_f64(b, 17),
            .gpsWeek = read_u16(b, 25),
            .gpsTimeMs = read_u32(b, 27),
            .sigmaEastM = read_f32(b, 31),
            .sigmaNorthM = read_f32(b, 35),
            .sigmaUpM = read_f32(b, 39),
        };
    }
};

// Record 70. ICD bytes 2..25 fixed, then a variable-length geoid model name.
//
// THE ONLY RECORD WHOSE HEIGHT IS ABOVE SEA LEVEL. Every other height a BD992
// reports -- records 2, 3, 35, 41, 62 -- is above the WGS-84 ellipsoid, and in
// southern California those differ by about 34.5 m. A consumer that shows the
// wrong one is not slightly off, it is a hundred feet off.
//
// The model name has NO length prefix and NO terminator: it runs from byte 26
// to the end of the record, and its length is the record length minus 24. That
// is the whole reason this record is variable-length, and it means the name
// cannot be read without trusting the record length byte.
struct LatLongMslHeight
{
    static constexpr RecordType kType = RecordType::LatLongMslHeight;
    static constexpr std::size_t kSize = 24;
    // Bounded by the record length byte: 255 - 24.
    static constexpr std::size_t kMaxModelLength = 231;

    double latitudeRad { 0.0 };
    double longitudeRad { 0.0 };
    double mslHeightM { 0.0 };

    // Not NUL terminated. Use modelName() rather than the array.
    std::uint8_t modelLength { 0 };
    std::array<char, kMaxModelLength> model {};

    constexpr std::span<const char> modelName() const
    {
        return std::span<const char>(model.data(), modelLength);
    }

    static constexpr Result<LatLongMslHeight> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<LatLongMslHeight>(b, kSize, kType))
        {
            return *err;
        }

        LatLongMslHeight out {};
        out.latitudeRad = read_f64(b, 0);
        out.longitudeRad = read_f64(b, 8);
        out.mslHeightM = read_f64(b, 16);

        const std::size_t nameLength = b.size() - kSize;
        if (nameLength > kMaxModelLength)
        {
            return length_mismatch(static_cast<std::uint8_t>(kType), static_cast<std::uint16_t>(b.size()));
        }

        out.modelLength = static_cast<std::uint8_t>(nameLength);
        for (std::size_t i = 0; i < nameLength; ++i)
        {
            out.model[i] = static_cast<char>(read_u8(b, kSize + i));
        }

        return out;
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

// Record 74. ICD bytes 2..39, but see the length note below.
//
// Record 12's layout, for the SECOND antenna. Same fields, same order, same
// units -- it is the heading antenna's own position quality, and it is how you
// tell "the attitude solution is bad" from "both antennas are bad".
//
// THE LENGTH DOES NOT MATCH THE ICD, and the mismatch is not a plain
// extension. The ICD prints a record length of 26h -- 38 body bytes ending in
// a two-byte epoch count at offset 36 -- and the bench receiver emits 42. The
// four extra bytes are NOT appended after the epoch count: offset 36 held a
// float, not a plausible count, and it was bit-for-bit equal to the range RMS
// at offset 0.
//
// Which four bytes moved cannot be settled from that capture, because the
// second antenna was disconnected and every field except the saturated range
// RMS read zero -- two different insertions produce identical bytes when
// everything is zero. So this parser reads only what is unambiguous, and
// epochCount is reported as ABSENT unless the record is exactly the ICD's 38
// bytes. Publishing the u16 at offset 36 regardless would have yielded 17027
// epochs for a single-epoch fix, which is the failure this avoids.
//
// TO SETTLE IT: connect a second antenna, let it produce non-zero sigmas, and
// see which fields land where.
struct SecondAntennaSigma
{
    static constexpr RecordType kType = RecordType::SecondAntennaSigma;
    static constexpr std::size_t kSize = 36;
    // The ICD's full length, epoch count included.
    static constexpr std::size_t kSizeWithEpochCount = 38;

    float rangeRms { 0.0F };
    float sigmaEastM { 0.0F };
    float sigmaNorthM { 0.0F };
    float covarianceEastNorth { 0.0F };
    float sigmaUpM { 0.0F };
    float semiMajorM { 0.0F };
    float semiMinorM { 0.0F };
    float orientationDeg { 0.0F };
    float unitVariance { 0.0F };

    bool hasEpochCount { false };
    std::uint16_t epochCount { 0 };

    static constexpr Result<SecondAntennaSigma> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<SecondAntennaSigma>(b, kSize, kType))
        {
            return *err;
        }

        SecondAntennaSigma out {};
        out.rangeRms = read_f32(b, 0);
        out.sigmaEastM = read_f32(b, 4);
        out.sigmaNorthM = read_f32(b, 8);
        out.covarianceEastNorth = read_f32(b, 12);
        out.sigmaUpM = read_f32(b, 16);
        out.semiMajorM = read_f32(b, 20);
        out.semiMinorM = read_f32(b, 24);
        out.orientationDeg = read_f32(b, 28);
        out.unitVariance = read_f32(b, 32);

        // Exactly, not at least -- see the header comment. A longer record is
        // a layout this build has not seen, and guessing where the count moved
        // to is worse than saying it is absent.
        if (b.size() == kSizeWithEpochCount)
        {
            out.hasEpochCount = true;
            out.epochCount = read_u16(b, 36);
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

// The same ceilings for the single-constellation records 13 and 14, whose
// entries are 3 and 8 bytes: (255 - 1) / 3 and (255 - 1) / 8. The ICD caps the
// COUNT field at 24 instead, but the length byte is the harder bound and the
// one a firmware cannot quietly exceed, so it is what the parsers enforce.
inline constexpr std::size_t kMaxGpsSvBrief = 84;
inline constexpr std::size_t kMaxGpsSvDetailed = 31;

// Record 13. ICD byte 2 is the count, then 3 bytes per satellite.
//
// The GPS-only sibling of record 33: no SV SYSTEM byte, because every entry is
// a GPS or SBAS PRN. On the receiver this bench was validated against it also
// listed a satellite that record 33 omitted -- PRN 1, flags 0x03, above the
// horizon and assigned a channel but not yet tracked -- so the two records are
// not redundant even when both are enabled.
struct SvBriefInfo
{
    static constexpr RecordType kType = RecordType::SvBriefInfo;
    static constexpr std::size_t kSize = 1;
    static constexpr std::size_t kEntrySize = 3;

    struct Entry
    {
        std::uint8_t prn { 0 };
        SvFlags flags {};
    };

    std::uint8_t count { 0 };
    std::array<Entry, kMaxGpsSvBrief> satellites {};

    constexpr std::span<const Entry> view() const
    {
        return std::span<const Entry>(satellites.data(), count);
    }

    static constexpr Result<SvBriefInfo> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<SvBriefInfo>(b, kSize, kType))
        {
            return *err;
        }

        SvBriefInfo out {};
        out.count = read_u8(b, 0);

        if (out.count > kMaxGpsSvBrief)
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
            out.satellites[i] = Entry { read_u8(b, at), SvFlags { read_u8(b, at + 1), read_u8(b, at + 2) } };
        }

        return out;
    }
};

// Record 14. ICD byte 2 is the count, then 8 bytes per satellite.
//
// The GPS-only sibling of record 34, and two bytes shorter per entry than it:
// no SV SYSTEM byte, and only two signal-to-noise figures where record 34
// carries three. Elevation and azimuth agree with record 34 exactly for every
// satellite both report; SNR L2 does not, because the two records label
// different signals -- so do not treat one as a cheaper form of the other.
struct SvDetailInfo
{
    static constexpr RecordType kType = RecordType::SvDetailInfo;
    static constexpr std::size_t kSize = 1;
    static constexpr std::size_t kEntrySize = 8;

    struct Entry
    {
        std::uint8_t prn { 0 };
        SvFlags flags {};
        std::uint8_t elevationDeg { 0 };
        std::uint16_t azimuthDeg { 0 };
        // dB times four, as in record 34. See SvDetail for why these stay
        // scaled in the struct.
        std::uint8_t snrFirstScaled { 0 };
        std::uint8_t snrSecondScaled { 0 };

        constexpr float snrFirstDb() const { return static_cast<float>(snrFirstScaled) * 0.25F; }
        constexpr float snrSecondDb() const { return static_cast<float>(snrSecondScaled) * 0.25F; }
    };

    std::uint8_t count { 0 };
    std::array<Entry, kMaxGpsSvDetailed> satellites {};

    constexpr std::span<const Entry> view() const
    {
        return std::span<const Entry>(satellites.data(), count);
    }

    static constexpr Result<SvDetailInfo> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<SvDetailInfo>(b, kSize, kType))
        {
            return *err;
        }

        SvDetailInfo out {};
        out.count = read_u8(b, 0);

        if (out.count > kMaxGpsSvDetailed)
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
            out.satellites[i] = Entry {
                .prn = read_u8(b, at),
                .flags = SvFlags { read_u8(b, at + 1), read_u8(b, at + 2) },
                .elevationDeg = read_u8(b, at + 3),
                .azimuthDeg = read_u16(b, at + 4),
                .snrFirstScaled = read_u8(b, at + 6),
                .snrSecondScaled = read_u8(b, at + 7),
            };
        }

        return out;
    }
};

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

// Record 48. The way past record 34's ceiling.
//
// Record 34 carries a count and 10 bytes per satellite in a body the length
// byte caps at 255, so it stops at 25 satellites. A BD992 tracking GPS,
// GLONASS, Galileo, BeiDou and SBAS routinely sees more than that, and record
// 34 simply truncates -- on the bench this record reported 31 satellites in
// the same epoch that record 34 reported 24, with entries identical for every
// satellite both listed.
//
// THE PAGING HERE IS NOT THE TRANSPORT PAGING of transport.h. That one splits
// one transmission across DCOL packets and is reassembled before any record is
// read; this one splits one logical satellite list across several complete,
// individually-framed GSOF records that all arrive inside the SAME
// transmission. So a caller sees N record 48s per epoch and has to join them
// itself -- which is why pageNumber and totalPages are surfaced rather than
// hidden.
//
// AN ENTRY IS A SIGNAL GROUP, NOT A SATELLITE, and joining the pages by PRN
// therefore loses data. On the bench, 31 entries covered 28 distinct
// satellites: GPS 4, BeiDou 19 and BeiDou 20 each appeared twice, with
// identical elevation and azimuth but a different SNR triple and different
// flags each time. A consumer keying a map by PRN silently keeps whichever
// entry it saw last. Concatenate the pages; do not deduplicate them.
//
// The SV SYSTEM byte also goes past the ICD's documented 0..5 here: the bench
// receiver listed a PRN 23 with system 10 and every other field zero. That is
// why `system` is a raw byte everywhere in this library and to_string() has an
// "unknown" answer rather than an assertion.
struct AllSvDetailedPage
{
    static constexpr RecordType kType = RecordType::AllSvDetailedPage;
    static constexpr std::size_t kSize = 3;
    static constexpr std::size_t kEntrySize = 10;

    // (255 - 3) / 10. A page holds fewer satellites than record 34 does,
    // because the version and page bytes come out of the same budget.
    static constexpr std::size_t kMaxPerPage = 25;

    std::uint8_t version { 0 };
    // The wire byte, kept whole: the two nibbles below are the ICD's reading of
    // it, and an unexpected version may well renumber them.
    std::uint8_t pageByte { 0 };
    std::uint8_t count { 0 };
    std::array<SvDetail, kMaxPerPage> satellites {};

    // Both are one-based on the wire: page 1 of 2, then page 2 of 2.
    constexpr std::uint8_t totalPages() const { return static_cast<std::uint8_t>(pageByte & 0x0Fu); }
    constexpr std::uint8_t pageNumber() const { return static_cast<std::uint8_t>((pageByte >> 4) & 0x0Fu); }
    constexpr bool isLastPage() const { return pageNumber() >= totalPages(); }

    constexpr std::span<const SvDetail> view() const
    {
        return std::span<const SvDetail>(satellites.data(), count);
    }

    static constexpr Result<AllSvDetailedPage> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<AllSvDetailedPage>(b, kSize, kType))
        {
            return *err;
        }

        AllSvDetailedPage out {};
        out.version = read_u8(b, 0);
        out.pageByte = read_u8(b, 1);
        out.count = read_u8(b, 2);

        if (out.count > kMaxPerPage)
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

// Record 28. ICD bytes 2..19.
//
// Eleven of its eighteen bytes are RESERVED in the ICD, and on the bench all
// eleven really were zero across every epoch of a 90-second capture -- which is
// the check that says the three fields that are NOT reserved are being read at
// the right offsets, because a layout off by one would have put a moving value
// in a reserved slot. They are kept as named `reservedN` members rather than
// dropped so the struct still describes all eighteen bytes.
//
// The cross-check that actually pins it down: RTK POSITION AGE tracked record
// 38's correctionAgeS exactly, at ten counts per second -- 0x46 here while
// record 38 read 7.00 s.
struct ReceiverDiagnostics
{
    static constexpr RecordType kType = RecordType::ReceiverDiagnostics;
    static constexpr std::size_t kSize = 18;

    std::uint8_t baseFlags { 0 };            // ICD 7
    // 0..255 across the full range, so a percentage is linkIntegrity * 100 / 256
    // rather than / 100. The ICD's own name for the field is the trap.
    std::uint8_t linkIntegrity { 0 };        // ICD 8
    std::uint8_t commonL1Svs { 0 };          // ICD 11
    std::uint8_t commonL2Svs { 0 };          // ICD 12
    std::uint8_t datalinkLatencyTenthsS { 0 };  // ICD 13
    std::uint8_t diffSvsInUse { 0 };         // ICD 15
    std::uint8_t rtkPositionAgeTenthsS { 0 };   // ICD 18

    constexpr float linkIntegrityPercent() const
    {
        return static_cast<float>(linkIntegrity) * (100.0F / 256.0F);
    }

    constexpr float datalinkLatencyS() const
    {
        return static_cast<float>(datalinkLatencyTenthsS) * 0.1F;
    }

    constexpr float rtkPositionAgeS() const
    {
        return static_cast<float>(rtkPositionAgeTenthsS) * 0.1F;
    }

    static constexpr Result<ReceiverDiagnostics> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<ReceiverDiagnostics>(b, kSize, kType))
        {
            return *err;
        }

        return ReceiverDiagnostics {
            .baseFlags = read_u8(b, 5),
            .linkIntegrity = read_u8(b, 6),
            .commonL1Svs = read_u8(b, 9),
            .commonL2Svs = read_u8(b, 10),
            .datalinkLatencyTenthsS = read_u8(b, 11),
            .diffSvsInUse = read_u8(b, 13),
            .rtkPositionAgeTenthsS = read_u8(b, 16),
        };
    }
};

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

// The values of PositionType::positionFixType, the ICD's list entire.
//
// It was previously the subset "a BD992 in a vehicle can produce", which was a
// guess and a wrong one: the first live receiver reported 33 within the hour,
// and 33 was not in it. The raw byte is always carried alongside so nothing was
// lost -- but a consumer switching on the enum saw Unknown for a perfectly
// ordinary RTX fix, which is the kind of gap that turns into "the GPS is
// broken" on a dashboard. The list is cheap; guessing which half of it matters
// is not.
//
// The gaps are the ICD's own: 34, 35, 45, 46 and 47 are RESERVED.
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
    OmniStarHpXp = 16,
    OmniStarHpG2 = 17,
    OmniStarG2 = 18,
    SynchronousRtx = 19,
    LowLatencyRtx = 20,
    OmniStarMultipleSource = 21,
    OmniStarL1Only = 22,
    // 23..30 are the INS forms of the types above. A BD992 has no IMU, so
    // these arrive only from an Applanix-style receiver on the same library.
    InsAutonomous = 23,
    InsSbas = 24,
    InsCodePhaseDgnss = 25,
    InsRtxCodePhase = 26,
    InsRtxCarrierPhase = 27,
    InsOmniStar = 28,
    InsRtk = 29,
    InsDeadReckoning = 30,
    RtxCodePhase = 31,
    RtxFastSync = 32,
    // What the first live BD992 in this tree actually reported, on a partly
    // occluded antenna with no base station: RTX converged, 4 cm sigmas.
    RtxFastLowLatency = 33,
    XFillRtx = 36,
    LowLatencyRtxRangePoint = 37,
    SynchronousRtxRangePoint = 38,
    LowLatencyRtxViewPoint = 39,
    SynchronousRtxViewPoint = 40,
    LowLatencyRtxFieldPoint = 41,
    SynchronousRtxFieldPoint = 42,
    OmniStarG2Plus = 43,
    OmniStarG4Plus = 44,
    L1sSlas = 48,
    InsXFillRtx = 49,
    Clas = 50,
    InsClas = 51,
    Has = 52,
    InsHas = 53,
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
// Signal integrity -- spoofing and ionosphere
// ============================================================================

// Which service authenticated a navigation message.
enum class NmaSource : std::uint8_t
{
    Osnma = 0,
    RtxNma = 1,
    QzNma = 2,
};

// Record 91. ICD bytes 2..8 fixed, then a variable number of variable-length
// entries.
//
// Says which satellites' broadcast navigation messages were cryptographically
// authenticated, and which FAILED. A failure is the signature of a spoofer:
// the position may look entirely healthy while being fabricated, so this is
// the one record whose "everything is fine" is worth publishing.
//
// TWO VARIABLE LENGTHS NEST HERE. The record holds `count` entries, and each
// entry names its own mask size, so an entry's length is 3 + 2 * maskBytes and
// the entries differ from each other -- on the bench, one record carried a
// 4-byte GPS mask, a 5-byte Galileo mask and an 8-byte BeiDou mask together.
// The walk therefore has to be a walk; there is no stride to multiply.
//
// AN ENTRY IS A (SOURCE, SIGNAL TYPE) PAIR, NOT A SOURCE. The ICD names three
// sources, which makes four entries look like a generous bound -- it is not.
// The same receiver went from one entry to three within minutes as more
// constellations came into view, and signal type runs 0..10, so the real
// ceiling is the record length and nothing smaller.
//
// So the masks live in ONE FLAT BUFFER sized to the record's own limit rather
// than in a fixed-size array per entry. A per-entry array has to guess a mask
// width -- 16 bytes covers BeiDou's 63 PRNs but not a QZSS mask indexed by
// absolute PRN -- and multiplying two guesses together is both far larger than
// any real record and still able to reject a legal one. This layout cannot
// reject anything a 255-byte record can express.
struct NavMessageAuth
{
    static constexpr RecordType kType = RecordType::NavMessageAuth;
    static constexpr std::size_t kSize = 7;

    // What is left of a 255-byte body after the fixed part, which bounds both
    // the entry count (at 3 bytes each) and the total mask storage.
    static constexpr std::size_t kMaxPayload = 248;
    static constexpr std::size_t kMaxEntries = kMaxPayload / 3;
    static constexpr std::size_t kMaxMaskStorage = kMaxPayload;

    struct Entry
    {
        std::uint8_t source { 0 };
        // 0..10 in the ICD, naming a constellation and signal together. Kept
        // raw: the list is receiver-specific and still growing.
        std::uint8_t signalType { 0 };
        std::uint8_t maskBytes { 0 };
        // Where this entry's authenticated mask starts in `masks`. The failed
        // mask follows it immediately, so it starts at maskAt + maskBytes.
        std::uint8_t maskAt { 0 };
    };

    std::uint16_t gpsWeek { 0 };
    std::uint32_t gpsTimeMs { 0 };
    std::uint8_t count { 0 };
    std::array<Entry, kMaxEntries> entries {};

    // Both masks of every entry, back to back in entry order. BIT 0 OF BYTE 0
    // IS PRN 1, so a mask is little-endian by bit while every scalar in GSOF is
    // big-endian by byte. Use the accessors rather than reading this.
    std::uint8_t maskLength { 0 };
    std::array<std::uint8_t, kMaxMaskStorage> masks {};

    constexpr std::span<const Entry> view() const
    {
        return std::span<const Entry>(entries.data(), count);
    }

    // `prn` is one-based, as the ICD numbers satellites. Out of range is false
    // rather than an error: a mask covers maskBytes * 8 PRNs and says nothing
    // about the rest.
    constexpr bool isAuthenticated(std::size_t entry, std::uint8_t prn) const
    {
        return maskBit(entry, prn, 0);
    }

    constexpr bool isFailed(std::size_t entry, std::uint8_t prn) const
    {
        return maskBit(entry, prn, 1);
    }

    // Whether any entry reported a failure. The question a consumer actually
    // has, and the one worth an alarm.
    constexpr bool anyFailed() const
    {
        for (std::size_t e = 0; e < count; ++e)
        {
            const std::size_t at = entries[e].maskAt + entries[e].maskBytes;
            for (std::size_t i = 0; i < entries[e].maskBytes; ++i)
            {
                if (masks[at + i] != 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    static constexpr Result<NavMessageAuth> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<NavMessageAuth>(b, kSize, kType))
        {
            return *err;
        }

        NavMessageAuth out {};
        out.gpsWeek = read_u16(b, 0);
        out.gpsTimeMs = read_u32(b, 2);
        out.count = read_u8(b, 6);

        if (out.count > kMaxEntries)
        {
            return length_mismatch(static_cast<std::uint8_t>(kType), out.count);
        }

        std::size_t at = kSize;
        std::size_t stored = 0;

        for (std::size_t i = 0; i < out.count; ++i)
        {
            // The three-byte entry header must be there before its mask size
            // can be believed.
            if (at + 3 > b.size())
            {
                return length_mismatch(static_cast<std::uint8_t>(kType), static_cast<std::uint16_t>(at));
            }

            Entry entry {};
            entry.source = read_u8(b, at);
            entry.signalType = read_u8(b, at + 1);
            entry.maskBytes = read_u8(b, at + 2);
            at += 3;

            const std::size_t both = 2u * static_cast<std::size_t>(entry.maskBytes);
            if (at + both > b.size() || stored + both > kMaxMaskStorage)
            {
                return length_mismatch(static_cast<std::uint8_t>(kType), static_cast<std::uint16_t>(at));
            }

            entry.maskAt = static_cast<std::uint8_t>(stored);
            for (std::size_t k = 0; k < both; ++k)
            {
                out.masks[stored + k] = read_u8(b, at + k);
            }
            stored += both;
            at += both;

            out.entries[i] = entry;
        }

        out.maskLength = static_cast<std::uint8_t>(stored);
        return out;
    }

  private:
    // `which` is 0 for the authenticated mask and 1 for the failed one, which
    // sits immediately after it.
    constexpr bool maskBit(std::size_t entry, std::uint8_t prn, std::size_t which) const
    {
        if (entry >= count || prn == 0)
        {
            return false;
        }
        const Entry& e = entries[entry];
        const std::size_t index = static_cast<std::size_t>(prn - 1) / 8;
        if (index >= e.maskBytes)
        {
            return false;
        }
        const std::size_t at = e.maskAt + which * e.maskBytes + index;
        return bit(masks[at], static_cast<unsigned>((prn - 1) % 8));
    }
};

// Where a receiver's ionospheric correction came from.
enum class IonoGuardSource : std::uint8_t
{
    Unknown = 0,
    RtkBase = 1,
    RoverComputed = 2,
    Rtx = 3,
    Invalid = 255,
};

// How disturbed the ionosphere is, per station or per satellite. Green through
// red, in the ICD's own order.
enum class IonoGuardLevel : std::uint8_t
{
    Green = 0,
    Yellow = 1,
    Orange = 2,
    Red = 3,
};

// Record 92. ICD bytes 2..11 fixed, then 3 bytes per satellite.
//
// Per-satellite ionospheric disturbance. A scintillating ionosphere degrades an
// RTK fix in a way that looks like nothing else in the telemetry -- the fix
// type stays fixed and the sigmas stay small right up until the solution walks.
//
// A receiver with no IonoGuard source reports source and geofence as 255 and a
// satellite count of zero, which is what the bench receiver did throughout, so
// the empty case is the one this parser has actually been exercised on.
struct IonoGuardInfo
{
    static constexpr RecordType kType = RecordType::IonoGuardInfo;
    static constexpr std::size_t kSize = 10;
    static constexpr std::size_t kEntrySize = 3;

    // (255 - 10) / 3.
    static constexpr std::size_t kMaxSatellites = 81;

    struct Entry
    {
        std::uint8_t system { 0 };   // the SvSystem numbering, plus 6 for NavIC
        std::uint8_t prn { 0 };
        std::uint8_t metric { 0 };   // IonoGuardLevel

        constexpr IonoGuardLevel level() const { return static_cast<IonoGuardLevel>(metric); }
    };

    std::uint16_t gpsWeek { 0 };
    std::uint32_t gpsTimeMs { 0 };
    std::uint8_t sourceRaw { 0 };
    std::uint8_t geofenceStatus { 0 };   // 0 inside, 1 outside, 255 unknown
    std::uint8_t stationActivityLevel { 0 };
    std::uint8_t count { 0 };
    std::array<Entry, kMaxSatellites> satellites {};

    constexpr IonoGuardSource source() const { return static_cast<IonoGuardSource>(sourceRaw); }

    constexpr std::span<const Entry> view() const
    {
        return std::span<const Entry>(satellites.data(), count);
    }

    static constexpr Result<IonoGuardInfo> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<IonoGuardInfo>(b, kSize, kType))
        {
            return *err;
        }

        IonoGuardInfo out {};
        out.gpsWeek = read_u16(b, 0);
        out.gpsTimeMs = read_u32(b, 2);
        out.sourceRaw = read_u8(b, 6);
        out.geofenceStatus = read_u8(b, 7);
        out.stationActivityLevel = read_u8(b, 8);
        out.count = read_u8(b, 9);

        if (out.count > kMaxSatellites)
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
            out.satellites[i] = Entry { read_u8(b, at), read_u8(b, at + 1), read_u8(b, at + 2) };
        }

        return out;
    }
};

// Record 96. ICD bytes 2..8.
//
// Record 92 counted, so a consumer that only wants "is the ionosphere quiet"
// does not have to subscribe to the per-satellite list. It carries no time of
// its own -- pair it with whatever else arrived in the same transmission.
struct IonoGuardSummary
{
    static constexpr RecordType kType = RecordType::IonoGuardSummary;
    static constexpr std::size_t kSize = 7;

    std::uint8_t sourceRaw { 0 };
    std::uint8_t geofenceStatus { 0 };
    std::uint8_t stationActivityLevel { 0 };
    std::uint8_t greenSvs { 0 };
    std::uint8_t yellowSvs { 0 };
    std::uint8_t orangeSvs { 0 };
    std::uint8_t redSvs { 0 };

    constexpr IonoGuardSource source() const { return static_cast<IonoGuardSource>(sourceRaw); }

    static constexpr Result<IonoGuardSummary> parse(std::span<const std::uint8_t> b)
    {
        if (const auto err = detail::require<IonoGuardSummary>(b, kSize, kType))
        {
            return *err;
        }

        return IonoGuardSummary {
            read_u8(b, 0), read_u8(b, 1), read_u8(b, 2), read_u8(b, 3),
            read_u8(b, 4), read_u8(b, 5), read_u8(b, 6),
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
