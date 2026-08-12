// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GSOF record parsers, against captures from real receivers.
//
// MOST OF THIS FILE RUNS AT COMPILE TIME. That is the claim the whole library
// is built to make: a GNSS field offset that is off by one produces a
// plausible latitude, not a crash, so it cannot be caught by looking at output
// and is not reliably caught by a runtime assertion someone stops running.
// Here, `cmake --build` is the check. If this file compiles, the offsets are
// right.
//
// The strongest evidence is not any single assertion, it is
// `test_records_agree_with_each_other`: three records from three separate
// captures describing the same physical situation. Records 35 and 41 report
// one base station; record 7 says the rover is 16 051 m north and 1 457 m west
// of it; records 2 and 3 place the rover, in two different coordinate systems,
// exactly there. A hand-written test vector cannot produce that, and a
// transposed field destroys it.

#include "gsof/record_iterator.h"
#include "gsof/records.h"
#include "golden/golden_records.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
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

template <std::size_t N>
constexpr std::span<const std::uint8_t> bytes(const std::array<std::uint8_t, N>& array)
{
    return std::span<const std::uint8_t>(array);
}

// ============================================================================
// Every record parses its golden body
// ============================================================================

#define GSOF_GOLDEN_PARSES(Name, golden) \
    static_assert(Name::parse(bytes(golden)).has_value(), #Name " must parse its captured body")

GSOF_GOLDEN_PARSES(PositionTime, golden::kPositionTime);
GSOF_GOLDEN_PARSES(LatLongHeight, golden::kLatLongHeight);
GSOF_GOLDEN_PARSES(EcefPosition, golden::kEcefPosition);
GSOF_GOLDEN_PARSES(EcefDelta, golden::kEcefDelta);
GSOF_GOLDEN_PARSES(TangentPlaneDelta, golden::kTangentPlaneDelta);
GSOF_GOLDEN_PARSES(Velocity, golden::kVelocity);
GSOF_GOLDEN_PARSES(DopInfo, golden::kDopInfo);
GSOF_GOLDEN_PARSES(ClockInfo, golden::kClockInfo);
GSOF_GOLDEN_PARSES(PositionVcv, golden::kPositionVcv);
GSOF_GOLDEN_PARSES(PositionSigma, golden::kPositionSigma);
GSOF_GOLDEN_PARSES(ReceiverSerial, golden::kReceiverSerial);
GSOF_GOLDEN_PARSES(CurrentTimeUtc, golden::kCurrentTimeUtc);
GSOF_GOLDEN_PARSES(AttitudeInfo, golden::kAttitudeInfo);
GSOF_GOLDEN_PARSES(AllSvBrief, golden::kAllSvBrief);
GSOF_GOLDEN_PARSES(AllSvDetailed, golden::kAllSvDetailed);
GSOF_GOLDEN_PARSES(ReceivedBase, golden::kReceivedBase);
GSOF_GOLDEN_PARSES(BatteryMemory, golden::kBatteryMemory);
GSOF_GOLDEN_PARSES(PositionType, golden::kPositionType);
GSOF_GOLDEN_PARSES(LbandStatus, golden::kLbandStatus);
GSOF_GOLDEN_PARSES(BasePosition, golden::kBasePosition);
GSOF_GOLDEN_PARSES(InsFullNav, golden::kInsFullNav);
GSOF_GOLDEN_PARSES(InsRms, golden::kInsRms);

#undef GSOF_GOLDEN_PARSES

// ============================================================================
// Record 1 -- and the week/time ordering that only this record has
// ============================================================================

constexpr PositionTime kTime = *PositionTime::parse(bytes(golden::kPositionTime));

static_assert(kTime.gpsWeek == 2225, "GPS week 2225 is 2022-08-28, when this was captured");
static_assert(kTime.gpsTimeMs == 143605000, "and 143605 s into that week");
static_assert(kTime.gpsTimeMs < 7 * 24 * 3600 * 1000,
              "the time of week must be inside a week -- swap it with gpsWeek and this fails");
static_assert(kTime.svsUsed == 25);
static_assert(kTime.positionFlags1 == 0xBF);

// 0xBF = 1011 1111. The flags are the reason the raw byte is kept as well.
static_assert(kTime.isNewPosition());
static_assert(kTime.hasClockFix());
static_assert(kTime.horizontalComputedHere());
static_assert(kTime.heightComputedHere());
static_assert(kTime.isWeightedLeastSquares());
static_assert(kTime.usedL1Pseudorange());
static_assert(!kTime.isDifferential(), "bit 6 is clear in 0xBF");
static_assert(kTime.isDifferentialPhase());

// ============================================================================
// Records 2, 3 -- position, in two coordinate systems
// ============================================================================

constexpr LatLongHeight kLlh = *LatLongHeight::parse(bytes(golden::kLatLongHeight));

static_assert(kLlh.latitudeRad == 0.7655233508990195);
static_assert(kLlh.longitudeRad == -1.3854359517010535);
static_assert(kLlh.heightM == 167.29419874649852);
// 43.86 N, 79.38 W: Richmond Hill, Ontario. Sanity bounds that a byte-swapped
// double could not possibly satisfy.
static_assert(kLlh.latitudeRad > 0.76 && kLlh.latitudeRad < 0.77);
static_assert(kLlh.longitudeRad > -1.39 && kLlh.longitudeRad < -1.38);

constexpr EcefPosition kEcef = *EcefPosition::parse(bytes(golden::kEcefPosition));

static_assert(kEcef.xM == 848943.2679713647);
static_assert(kEcef.yM == -4527385.996442103);
static_assert(kEcef.zM == 4397104.473719332);

// ============================================================================
// Record 8 -- and the trailing field that has no flag
// ============================================================================

constexpr Velocity kVel = *Velocity::parse(bytes(golden::kVelocity));

static_assert(golden::kVelocity.size() == Velocity::kSize, "the capture is the short form");
static_assert(!kVel.hasLocalHeading, "so there is no local heading in it");
static_assert(kVel.isValid());
static_assert(kVel.headingRad == 2.06223201751709F);
static_assert(kVel.horizontalSpeedMps < 0.01F, "the receiver was stationary");

// The long form, built by appending a float. Nothing in the record says the
// field is there; only the length does.
constexpr std::array<std::uint8_t, Velocity::kSizeWithLocalHeading> kVelocityLong = [] {
    std::array<std::uint8_t, Velocity::kSizeWithLocalHeading> out {};
    for (std::size_t i = 0; i < Velocity::kSize; ++i)
    {
        out[i] = golden::kVelocity[i];
    }
    // 3.0f, big endian.
    out[13] = 0x40;
    out[14] = 0x40;
    out[15] = 0x00;
    out[16] = 0x00;
    return out;
}();

constexpr Velocity kVelLong = *Velocity::parse(bytes(kVelocityLong));

static_assert(kVelLong.hasLocalHeading, "the long form is recognised by its length alone");
static_assert(kVelLong.localHeadingRad == 3.0F);
static_assert(kVelLong.headingRad == kVel.headingRad, "and the leading fields are unmoved");

// ============================================================================
// Record 27 -- attitude, and the variance block that also has no flag
// ============================================================================

constexpr AttitudeInfo kAtt = *AttitudeInfo::parse(bytes(golden::kAttitudeInfo));

static_assert(golden::kAttitudeInfo.size() == AttitudeInfo::kSizeWithVariance);
static_assert(kAtt.hasVariance);
static_assert(kAtt.svsUsed == 27);
static_assert(kAtt.pitchRad == -0.0069243321593608275);
static_assert(kAtt.yawRad == 3.131870495343419);
static_assert(kAtt.rollRad == 3.135750624153477);
// Pitch, yaw, roll -- in that order on the wire. Yaw and roll are both near pi
// here and pitch is near zero, so a pitch/yaw transposition is caught by the
// magnitude alone.
static_assert(kAtt.pitchRad > -0.01 && kAtt.pitchRad < 0.0);
static_assert(kAtt.yawRad > 3.0 && kAtt.yawRad < 3.2);
static_assert(kAtt.pdopScaled == 10 && kAtt.pdop() == 1.0);

// Truncated to the pre-4.20 length: same fields, no variance.
constexpr AttitudeInfo kAttShort =
    *AttitudeInfo::parse(std::span<const std::uint8_t>(golden::kAttitudeInfo).first(AttitudeInfo::kSize));

static_assert(!kAttShort.hasVariance);
static_assert(kAttShort.yawRad == kAtt.yawRad, "and every field before the variance block is unchanged");

// ============================================================================
// Records 33, 34 -- counted lists
// ============================================================================

constexpr AllSvBrief kBrief = *AllSvBrief::parse(bytes(golden::kAllSvBrief));

static_assert(kBrief.count == 25);
static_assert(golden::kAllSvBrief.size() == 1 + 25 * AllSvBrief::kEntrySize,
              "the count and the record length are two statements of the same fact");
static_assert(kBrief.satellites[0].prn == 11);
static_assert(kBrief.satellites[0].system == static_cast<std::uint8_t>(SvSystem::Gps));
static_assert(kBrief.view().size() == 25);

constexpr AllSvDetailed kDetail = *AllSvDetailed::parse(bytes(golden::kAllSvDetailed));

static_assert(kDetail.count == 24);
static_assert(golden::kAllSvDetailed.size() == 1 + 24 * AllSvDetailed::kEntrySize);
static_assert(kDetail.satellites[0].prn == 23);
static_assert(kDetail.satellites[0].elevationDeg == 19);
static_assert(kDetail.satellites[0].azimuthDeg == 263);
// SNR is dB times four on the wire. 0x58 == 88 == 22 dB.
static_assert(kDetail.satellites[0].snrFirstScaled == 0x58);
static_assert(kDetail.satellites[0].snrFirstDb() == 22.0F);

// ============================================================================
// Records 38, 40, 41 -- quality and corrections
// ============================================================================

constexpr PositionType kPosType = *PositionType::parse(bytes(golden::kPositionType));

static_assert(kPosType.errorScale == 1.0F);
static_assert(kPosType.positionFixTypeRaw == 29, "INS RTK, which is what an APX-60 reports");
static_assert(kPosType.isRtkFixed(), "bit 1 of 0x0A");
static_assert(!kPosType.isNetworkSolution(), "bit 0 of 0x0A");
static_assert(kPosType.initialisationIntegrity() == 2, "bits 2-3 of 0x0A");

constexpr LbandStatus kLband = *LbandStatus::parse(bytes(golden::kLbandStatus));

static_assert(kLband.satelliteName[0] == 'R' && kLband.satelliteName[1] == 'T' &&
              kLband.satelliteName[2] == 'X' && kLband.satelliteName[3] == 'N' &&
              kLband.satelliteName[4] == 'A');
static_assert(kLband.satelliteBitRate == 2400);
// Trimble RTX rides on L-band around 1.5 GHz. A field read at the wrong offset
// is not going to land in that window.
static_assert(kLband.nominalFrequencyMhz > 1500.0F && kLband.nominalFrequencyMhz < 1600.0F);

constexpr BasePosition kBase = *BasePosition::parse(bytes(golden::kBasePosition));
constexpr ReceivedBase kRecvBase = *ReceivedBase::parse(bytes(golden::kReceivedBase));

static_assert(kBase.gpsWeek == 2225, "week first in record 41, unlike record 1");
static_assert(kBase.baseQuality == 4);

// TWO CAPTURES, TWO RECORD LAYOUTS, ONE BASE STATION. Records 35 and 41 encode
// the base position at different offsets in differently shaped records; if
// either layout is wrong these cannot agree bit for bit.
static_assert(kBase.latitudeRad == kRecvBase.latitudeRad);
static_assert(kBase.longitudeRad == kRecvBase.longitudeRad);
static_assert(kBase.heightM == kRecvBase.heightM);
static_assert(kRecvBase.baseName[0] == 'R' && kRecvBase.baseName[3] == 'M');

// ============================================================================
// Records 16, 15, 9, 10, 11, 12, 37 -- the simple ones
// ============================================================================

constexpr CurrentTimeUtc kUtc = *CurrentTimeUtc::parse(bytes(golden::kCurrentTimeUtc));
static_assert(kUtc.gpsWeek == 2225);
static_assert(kUtc.utcOffsetS == 18, "GPS minus UTC has been 18 s since 2017");
static_assert(kUtc.isTimeValid() && kUtc.isUtcOffsetValid());

constexpr ReceiverSerial kSerial = *ReceiverSerial::parse(bytes(golden::kReceiverSerial));
static_assert(kSerial.serialNumber == 573901879);

constexpr DopInfo kDop = *DopInfo::parse(bytes(golden::kDopInfo));
static_assert(kDop.pdop == 0.9000375270843506F);
// PDOP is the worst of them by construction, so this ordering is a property of
// the quantity rather than of this capture.
static_assert(kDop.pdop > kDop.hdop && kDop.pdop > kDop.vdop);

constexpr ClockInfo kClock = *ClockInfo::parse(bytes(golden::kClockInfo));
static_assert(kClock.isClockOffsetValid() && kClock.isFrequencyOffsetValid());
static_assert(kClock.receiverClockOffsetMs > 0.0 && kClock.receiverClockOffsetMs < 1.0);

constexpr PositionVcv kVcv = *PositionVcv::parse(bytes(golden::kPositionVcv));
static_assert(kVcv.unitVariance == 1.0F);
static_assert(kVcv.epochCount == 0);
// A covariance matrix has a non-negative diagonal. Reading the off-diagonal
// terms at the diagonal's offsets would not reliably break this, but reading
// floats as the wrong endianness would.
static_assert(kVcv.xx > 0.0F && kVcv.yy > 0.0F && kVcv.zz > 0.0F);

constexpr PositionSigma kSigma = *PositionSigma::parse(bytes(golden::kPositionSigma));
static_assert(kSigma.unitVariance == 1.0F);
static_assert(kSigma.sigmaEastM == 0.20046113431453705F);
// The error ellipse's semi-major axis is by definition the larger one, and at
// least as large as either component sigma.
static_assert(kSigma.semiMajorM >= kSigma.semiMinorM);
static_assert(kSigma.semiMajorM >= kSigma.sigmaNorthM);

constexpr BatteryMemory kBattery = *BatteryMemory::parse(bytes(golden::kBatteryMemory));
static_assert(kBattery.batteryPercent == 100);

// ============================================================================
// Records 49, 50 -- degrees, where 2 and 41 were radians
// ============================================================================

constexpr InsFullNav kNav = *InsFullNav::parse(bytes(golden::kInsFullNav));

// Week 2080 -- these two come from an APX-18 session predating the applus60
// captures the rest of the file uses.
static_assert(kNav.gpsWeek == 2080, "week first");
static_assert(kNav.gpsTimeMs < 7 * 24 * 3600 * 1000, "and the time of week is inside a week");
static_assert(kNav.latitudeDeg > 43.0 && kNav.latitudeDeg < 44.0,
              "DEGREES here -- in record 2 the same place is 0.766 radians");
static_assert(kNav.longitudeDeg > -80.0 && kNav.longitudeDeg < -79.0);

constexpr InsRms kRms = *InsRms::parse(bytes(golden::kInsRms));
static_assert(kRms.gpsWeek == 2080);
static_assert(kRms.positionRmsNorthM > 0.0F && kRms.positionRmsNorthM < 10.0F);

// ============================================================================
// Malformed input -- at compile time where the shape allows
// ============================================================================

static_assert(LatLongHeight::parse(std::span<const std::uint8_t>(golden::kLatLongHeight).first(23))
                  .error()
                  .kind == ErrorKind::Truncated,
              "one byte short is refused");
static_assert(LatLongHeight::parse(std::span<const std::uint8_t>()).error().kind == ErrorKind::Truncated);
static_assert(LatLongHeight::parse(std::span<const std::uint8_t>(golden::kLatLongHeight).first(23))
                  .error()
                  .recordType == 2,
              "and the error says which record it was");

// A record longer than the ICD says is NOT an error. Trimble extends records
// in place, and a parser that refused the tail would turn a firmware update
// into a total outage for that message.
constexpr std::array<std::uint8_t, 30> kLlhWithTail = [] {
    std::array<std::uint8_t, 30> out {};
    for (std::size_t i = 0; i < golden::kLatLongHeight.size(); ++i)
    {
        out[i] = golden::kLatLongHeight[i];
    }
    return out;
}();

static_assert(LatLongHeight::parse(bytes(kLlhWithTail)).has_value(), "a longer record is accepted");
static_assert(LatLongHeight::parse(bytes(kLlhWithTail))->latitudeRad == kLlh.latitudeRad,
              "and reads the same, from the same offsets");

// The count and the length must agree.
constexpr std::array<std::uint8_t, 9> kBriefLies { 25, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
static_assert(AllSvBrief::parse(bytes(kBriefLies)).error().kind == ErrorKind::LengthMismatch,
              "a count of 25 in a record with room for two is refused");

constexpr std::array<std::uint8_t, 1> kBriefEmpty { 0 };
static_assert(AllSvBrief::parse(bytes(kBriefEmpty)).has_value(), "zero satellites is legal");
static_assert(AllSvBrief::parse(bytes(kBriefEmpty))->count == 0);

// ============================================================================
// Run-time: the parts that need <cmath>, and the record walk
// ============================================================================

void test_records_agree_with_each_other()
{
    // Record 3 places the rover in ECEF. Record 2 places it in geodetic
    // coordinates. Convert record 2 and the two must land in the same place.
    constexpr double kA = 6378137.0;              // WGS-84 semi-major axis
    constexpr double kF = 1.0 / 298.257223563;    // flattening
    const double e2 = kF * (2.0 - kF);

    const double sinLat = std::sin(kLlh.latitudeRad);
    const double cosLat = std::cos(kLlh.latitudeRad);
    const double n = kA / std::sqrt(1.0 - e2 * sinLat * sinLat);

    const double x = (n + kLlh.heightM) * cosLat * std::cos(kLlh.longitudeRad);
    const double y = (n + kLlh.heightM) * cosLat * std::sin(kLlh.longitudeRad);
    const double z = (n * (1.0 - e2) + kLlh.heightM) * sinLat;

    const double error = std::sqrt((x - kEcef.xM) * (x - kEcef.xM) + (y - kEcef.yM) * (y - kEcef.yM) +
                                   (z - kEcef.zM) * (z - kEcef.zM));

    check(error < 1.0,
          "records 2 and 3 place the rover within a metre of each other (got " + std::to_string(error) + " m)");

    // Record 7 is the baseline from the base to the rover in the local tangent
    // plane. Records 2 and 41 give both endpoints, so the great-circle-ish
    // distance between them must match record 7's north/east magnitude.
    constexpr TangentPlaneDelta kDelta = *TangentPlaneDelta::parse(bytes(golden::kTangentPlaneDelta));

    const double meanLat = 0.5 * (kLlh.latitudeRad + kBase.latitudeRad);
    const double northM = (kLlh.latitudeRad - kBase.latitudeRad) * (kA * (1.0 - e2) /
                          std::pow(1.0 - e2 * std::sin(meanLat) * std::sin(meanLat), 1.5));
    const double eastM = (kLlh.longitudeRad - kBase.longitudeRad) * (kA / std::sqrt(1.0 - e2 * std::sin(meanLat) *
                          std::sin(meanLat))) * std::cos(meanLat);

    check(std::abs(northM - kDelta.northM) < 50.0,
          "record 7's northing matches the record 2 to record 41 baseline (got " + std::to_string(northM) +
              " vs " + std::to_string(kDelta.northM) + ")");
    check(std::abs(eastM - kDelta.eastM) < 50.0,
          "record 7's easting matches the record 2 to record 41 baseline (got " + std::to_string(eastM) +
              " vs " + std::to_string(kDelta.eastM) + ")");

    // And record 6 is the same baseline in ECEF, so it must have the same
    // length as record 7's.
    constexpr EcefDelta kEcefDelta = *EcefDelta::parse(bytes(golden::kEcefDelta));

    const double ecefLength = std::sqrt(kEcefDelta.deltaXM * kEcefDelta.deltaXM +
                                        kEcefDelta.deltaYM * kEcefDelta.deltaYM +
                                        kEcefDelta.deltaZM * kEcefDelta.deltaZM);
    const double enuLength = std::sqrt(kDelta.eastM * kDelta.eastM + kDelta.northM * kDelta.northM +
                                       kDelta.upM * kDelta.upM);

    check(std::abs(ecefLength - enuLength) < 1.0,
          "records 6 and 7 are the same baseline in two frames (got " + std::to_string(ecefLength) + " vs " +
              std::to_string(enuLength) + ")");
}

void test_ecef_magnitude_is_an_earth_radius()
{
    const double r = std::sqrt(kEcef.xM * kEcef.xM + kEcef.yM * kEcef.yM + kEcef.zM * kEcef.zM);
    check(r > 6.35e6 && r < 6.39e6,
          "the ECEF position has the magnitude of an Earth radius (got " + std::to_string(r) + " m)");
}

void test_satellite_list_is_plausible()
{
    // Elevation is an angle above the horizon, azimuth a bearing. Both have
    // hard physical limits that a misread byte violates immediately.
    for (const SvDetail& sv : kDetail.view())
    {
        check(sv.elevationDeg <= 90, "every satellite elevation is at most 90 degrees");
        check(sv.azimuthDeg <= 360, "every satellite azimuth is at most 360 degrees");
        check(sv.prn >= 1 && sv.prn <= 210, "every PRN is in the assigned range");
    }
}

// ============================================================================
// Record iteration
// ============================================================================

// A payload holding three records back to back, built from the golden bodies.
std::vector<std::uint8_t> multiRecordPayload()
{
    std::vector<std::uint8_t> payload;

    const auto append = [&payload](std::uint8_t type, std::span<const std::uint8_t> body) {
        payload.push_back(type);
        payload.push_back(static_cast<std::uint8_t>(body.size()));
        payload.insert(payload.end(), body.begin(), body.end());
    };

    append(1, bytes(golden::kPositionTime));
    append(2, bytes(golden::kLatLongHeight));
    append(8, bytes(golden::kVelocity));

    return payload;
}

void test_iterator_walks_every_record()
{
    const std::vector<std::uint8_t> payload = multiRecordPayload();

    RecordIterator it(payload);
    std::vector<std::uint8_t> seen;

    while (!it.done())
    {
        const Result<RawRecord> record = it.next();
        check(record.has_value(), "each record in a well-formed payload parses");
        if (!record.has_value())
        {
            break;
        }
        seen.push_back(record->type);
        check(is_known_record(record->type), "and is a type we model");
    }

    check(seen == std::vector<std::uint8_t> { 1, 2, 8 }, "all three records, in order");
    check(it.offset() == payload.size(), "and the whole payload was consumed");
}

void test_an_unknown_record_does_not_stop_the_walk()
{
    // THE case that matters. Enabling one unmodelled message on the receiver
    // must not silently disable every message the receiver sends after it.
    std::vector<std::uint8_t> payload;
    payload.push_back(1);
    payload.push_back(static_cast<std::uint8_t>(golden::kPositionTime.size()));
    payload.insert(payload.end(), golden::kPositionTime.begin(), golden::kPositionTime.end());

    // Record 92, IonoGuard: real, documented, and not in our table.
    payload.push_back(92);
    payload.push_back(6);
    payload.insert(payload.end(), { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11 });

    payload.push_back(2);
    payload.push_back(static_cast<std::uint8_t>(golden::kLatLongHeight.size()));
    payload.insert(payload.end(), golden::kLatLongHeight.begin(), golden::kLatLongHeight.end());

    RecordIterator it(payload);
    std::vector<std::uint8_t> seen;
    int unknowns = 0;

    while (!it.done())
    {
        const Result<RawRecord> record = it.next();
        if (!record.has_value())
        {
            break;
        }
        seen.push_back(record->type);

        const Result<void> visited = visit_record(*record, [](const auto&) {});
        if (!visited.has_value() && visited.error().kind == ErrorKind::UnknownRecord)
        {
            ++unknowns;
            check(!is_known_record(record->type), "an unparseable type is not in the table");
            check(record->body.size() == 6, "and its bytes are still available to pass through");
        }
    }

    check(seen == std::vector<std::uint8_t> { 1, 92, 2 }, "the record behind the unknown one is still found");
    check(unknowns == 1, "exactly one record was unknown");
}

void test_a_record_longer_than_the_payload_stops_the_walk()
{
    // The signature of a page lost in reassembly: the payload is short, and
    // the last record is the one that notices. Everything after it is a guess,
    // so the walk stops rather than resynchronising on a byte that might be a
    // record type.
    std::vector<std::uint8_t> payload { 1, 10, 0, 0, 0, 0, 0, 0, 0, 0 };  // claims 10, has 8

    RecordIterator it(payload);
    const Result<RawRecord> record = it.next();

    check(!record.has_value() && record.error().kind == ErrorKind::LengthMismatch,
          "a record claiming more bytes than the payload holds is refused");
    check(record.error().recordType == 1, "and names the record that lied");
    check(it.done(), "and the walk ends there");
}

void test_a_trailing_fragment_is_not_a_record()
{
    std::vector<std::uint8_t> payload = multiRecordPayload();
    payload.push_back(9);   // a lone type byte with no length behind it

    RecordIterator it(payload);
    int found = 0;
    while (!it.done())
    {
        if (it.next().has_value())
        {
            ++found;
        }
        else
        {
            break;
        }
    }

    check(found == 3, "three whole records, and the dangling byte is not a fourth");
    check(it.offset() < payload.size(), "the fragment is left unconsumed, and visibly so");
}

void test_visit_dispatches_to_the_right_type()
{
    const std::vector<std::uint8_t> payload = multiRecordPayload();

    RecordIterator it(payload);
    int positionTimes = 0;
    int latLongHeights = 0;
    int velocities = 0;
    double seenLatitude = 0.0;

    while (!it.done())
    {
        const Result<RawRecord> record = it.next();
        if (!record.has_value())
        {
            break;
        }

        const Result<void> visited = visit_record(*record, [&](const auto& held) {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, PositionTime>)
            {
                ++positionTimes;
            }
            else if constexpr (std::is_same_v<Held, LatLongHeight>)
            {
                ++latLongHeights;
                seenLatitude = held.latitudeRad;
            }
            else if constexpr (std::is_same_v<Held, Velocity>)
            {
                ++velocities;
            }
        });

        check(visited.has_value(), "every record in the payload is visited");
    }

    check(positionTimes == 1 && latLongHeights == 1 && velocities == 1,
          "each record reached exactly the overload for its own type");
    check(seenLatitude == kLlh.latitudeRad, "and carried its parsed value");
}

void test_record_names_and_known_set()
{
    check(std::string(record_name(RecordType::LatLongHeight)) == "lat_long_height", "records are named");
    check(std::string(record_name(RecordType::AttitudeInfo)) == "attitude_info", "records are named");
    check(is_known_record(27), "record 27 is modelled");
    check(!is_known_record(92), "record 92 is not");
    check(!is_known_record(0), "and neither is 0");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    test_records_agree_with_each_other();
    test_ecef_magnitude_is_an_earth_radius();
    test_satellite_list_is_plausible();
    test_iterator_walks_every_record();
    test_an_unknown_record_does_not_stop_the_walk();
    test_a_record_longer_than_the_payload_stops_the_walk();
    test_a_trailing_fragment_is_not_a_record();
    test_visit_dispatches_to_the_right_type();
    test_record_names_and_known_set();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all GSOF record checks passed");
    return 0;
}
