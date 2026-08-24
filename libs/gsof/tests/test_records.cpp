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
// ============================================================================
// The records added for a BD992: structure, not offsets
// ============================================================================
//
// THESE VECTORS ARE SYNTHETIC AND SAY SO. They are built here, by hand, which
// means they cannot do the job golden_records.h does -- a vector authored from
// the same reading of the ICD as the parser agrees with the parser by
// construction, including where both are wrong, so none of this validates a
// field OFFSET against reality.
//
// What they do validate is the parser's own arithmetic: nested variable
// lengths, counts that must agree with a record length, a trailing field
// distinguished only by how long the record is, and a name whose length is the
// record length minus the fixed part. Those are logic, and logic can be checked
// against bytes that were written to exercise it.
//
// The offset validation for records 13, 14, 28, 48, 62, 70, 74, 91, 92 and 96
// was done against a live receiver and is NOT in this repository: that capture
// was taken at a private location and the position is recoverable from it, both
// from the position records directly and from the satellite azimuths and
// elevations against the timestamp. Re-capture somewhere neutral to check it in
// -- libs/gsof/tests/golden/README.md has the procedure, and
// tools/gen_golden_bd992.py is the generator.

// ---- Record 91: two variable lengths nested ------------------------------
//
// The record holds `count` entries and each entry names its own mask size, so
// an entry is 3 + 2 * maskBytes and no two need be the same length. There is no
// stride to multiply, and mis-sizing any entry lands the next one's header in
// the middle of a mask. Three entries of three different widths is the shape
// that catches it; one entry, or three of equal width, does not.
constexpr std::array<std::uint8_t, 50> makeNavMessageAuth(bool withFailure)
{
    std::array<std::uint8_t, 50> out {};
    // week 2433, time of week, three entries.
    out[0] = 0x09; out[1] = 0x81;
    out[2] = 0x04; out[3] = 0xEC; out[4] = 0xE1; out[5] = 0x28;
    out[6] = 3;

    // Entry 0: RTX-NMA, signal 0, a four-byte mask. PRN 2 authenticated --
    // bit 1 of byte 0, because bit 0 is PRN 1.
    out[7] = 1; out[8] = 0; out[9] = 4;
    out[10] = 0x02;
    // out[14..17] is entry 0's failed mask, left clear.

    // Entry 1: OSNMA, signal 4, a five-byte mask. PRN 1 authenticated.
    out[18] = 0; out[19] = 4; out[20] = 5;
    out[21] = 0x01;
    // out[26..30] is entry 1's failed mask, left clear.

    // Entry 2: RTX-NMA, signal 6, an eight-byte mask -- BeiDou needs eight to
    // reach PRN 63. PRN 44 authenticated: byte 5, bit 3.
    out[31] = 1; out[32] = 6; out[33] = 8;
    out[39] = 0x08;
    if (withFailure)
    {
        // PRN 1 FAILED authentication, in entry 2's failed mask.
        out[42] = 0x01;
    }
    return out;
}

constexpr std::array<std::uint8_t, 50> kNmaClean = makeNavMessageAuth(false);
constexpr std::array<std::uint8_t, 50> kNmaFailed = makeNavMessageAuth(true);

constexpr NavMessageAuth kNma = *NavMessageAuth::parse(bytes(kNmaClean));

static_assert(kNma.count == 3);
static_assert(kNma.entries[0].maskBytes == 4);
static_assert(kNma.entries[1].maskBytes == 5);
static_assert(kNma.entries[2].maskBytes == 8);
static_assert(kNma.maskLength == 2 * (4 + 5 + 8), "both masks of all three entries, packed");
static_assert(kNmaClean.size() == NavMessageAuth::kSize + 3 * 3 + kNma.maskLength,
              "and the whole record is accounted for -- a mis-sized entry lands elsewhere");

static_assert(kNma.entries[0].source == static_cast<std::uint8_t>(NmaSource::RtxNma));
static_assert(kNma.entries[1].source == static_cast<std::uint8_t>(NmaSource::Osnma));

// BIT 0 OF BYTE 0 IS PRN 1: the masks are little-endian by bit where every
// scalar in GSOF is big-endian by byte. Reading them the other way round moves
// every PRN, and reading the wrong entry's mask moves them between
// constellations.
static_assert(kNma.isAuthenticated(0, 2));
static_assert(!kNma.isAuthenticated(0, 1), "PRN 1 is set in entry 1, not entry 0");
static_assert(kNma.isAuthenticated(1, 1));
static_assert(kNma.isAuthenticated(2, 44));
static_assert(!kNma.isAuthenticated(1, 44), "a five-byte mask cannot even address PRN 44");
static_assert(!kNma.isAuthenticated(0, 33), "nor a four-byte one PRN 33");
static_assert(!kNma.isAuthenticated(0, 0), "PRNs are one-based");
static_assert(!kNma.isAuthenticated(3, 2), "and an entry index past the count is false");

// A failure anywhere is the alarm, and it must not be swamped by the
// authenticated bits sitting next to it in the same buffer.
static_assert(!kNma.anyFailed());
static_assert(NavMessageAuth::parse(bytes(kNmaFailed))->anyFailed());
static_assert(NavMessageAuth::parse(bytes(kNmaFailed))->isFailed(2, 1));
static_assert(!NavMessageAuth::parse(bytes(kNmaFailed))->isFailed(0, 1),
              "the failure belongs to entry 2 alone");
static_assert(NavMessageAuth::parse(bytes(kNmaFailed))->isAuthenticated(2, 44),
              "and the authenticated mask beside it is unchanged");

// An entry whose mask runs past the record is refused rather than read.
constexpr std::array<std::uint8_t, 12> kNmaShort {
    0x09, 0x81, 0x04, 0xEC, 0xE1, 0x28, 0x01, 0x01, 0x00, 0x08, 0x00, 0x00,
};
static_assert(NavMessageAuth::parse(bytes(kNmaShort)).error().kind == ErrorKind::LengthMismatch,
              "an eight-byte mask in a record with room for two bytes is refused");

// ---- Record 38: the fix type list must stay the ICD's entire -------------
//
// This existed as a trimmed subset -- "the values a BD992 in a vehicle can
// produce" -- and a BD992 in a vehicle produced one of the missing ones inside
// an hour. The raw byte meant nothing was lost, but a consumer switching on the
// enum saw Unknown for an ordinary RTX fix. This is the guard against trimming
// it again.
constexpr std::array<std::uint8_t, 26> makePositionType(std::uint8_t fixType)
{
    std::array<std::uint8_t, 26> out {};
    out[25] = fixType;
    return out;
}

static_assert(PositionType::parse(bytes(makePositionType(33)))->positionFixType() ==
                  PositionFixType::RtxFastLowLatency,
              "33 is RTX Fast in Low Latency mode, and was once missing");
static_assert(PositionType::parse(bytes(makePositionType(19)))->positionFixType() ==
              PositionFixType::SynchronousRtx);
static_assert(PositionType::parse(bytes(makePositionType(53)))->positionFixType() ==
              PositionFixType::InsHas);
static_assert(PositionType::parse(bytes(makePositionType(9)))->positionFixType() ==
              PositionFixType::FixedRtk);

// The ICD reserves these, so they must NOT collide with a named value -- an
// unrecognised type has to read as unrecognised rather than as "no fix".
static_assert(PositionType::parse(bytes(makePositionType(34)))->positionFixTypeRaw == 34);
static_assert(PositionType::parse(bytes(makePositionType(34)))->positionFixType() !=
                  PositionFixType::NoFixOrOld,
              "a reserved value is not silently flattened onto 0");

// ---- Record 48: paging inside a record ------------------------------------
//
// Not the transport paging of transport.h. Several complete, separately framed
// GSOF 48 records arrive in one transmission, each naming its page in one byte
// split into two nibbles -- which is exactly the kind of field a parser gets
// backwards while still producing plausible small numbers.
constexpr std::array<std::uint8_t, 13> kSvPage {
    0x01,        // version
    0x22,        // page 2 of 2: high nibble is the page, low nibble the total
    0x01,        // one satellite
    0x09, 0x02, 0x8F, 0x04, 0x3F, 0x00, 0x39, 0x99, 0xA4, 0x00,
};

constexpr AllSvDetailedPage kPage = *AllSvDetailedPage::parse(bytes(kSvPage));

static_assert(kPage.pageNumber() == 2 && kPage.totalPages() == 2, "high nibble is the page");
static_assert(kPage.isLastPage());
static_assert(AllSvDetailedPage::parse(bytes(std::array<std::uint8_t, 3> { 0x01, 0x12, 0x00 }))
                  ->pageNumber() == 1,
              "and 0x12 is page 1 of 2, not page 2 of 1");
static_assert(kPage.count == 1);
static_assert(kSvPage.size() == AllSvDetailedPage::kSize + AllSvDetailedPage::kEntrySize,
              "the count and the record length are two statements of the same fact");
static_assert(kPage.satellites[0].prn == 9);
static_assert(kPage.satellites[0].elevationDeg == 63 && kPage.satellites[0].azimuthDeg == 57);

// A count the record has no room for is refused, as in records 33 and 34.
constexpr std::array<std::uint8_t, 5> kPageLies { 0x01, 0x11, 0x09, 0x00, 0x00 };
static_assert(AllSvDetailedPage::parse(bytes(kPageLies)).error().kind == ErrorKind::LengthMismatch);

// ---- Record 70: a name whose length is the record's ------------------------
//
// The geoid model name has no length prefix and no terminator. It runs from
// byte 26 to the end of the record, so its length is the record length minus
// 24 -- which means the name cannot be read without trusting the length byte,
// and a parser that assumed a fixed width would read the wrong thing on any
// receiver using a different model.
constexpr std::array<std::uint8_t, 29> kMsl {
    // Three doubles, then "EGM96" with nothing marking where it starts or ends.
    0x3F, 0xE2, 0xD3, 0x7F, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x6C, 0xCD, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x78, 0x65, 0xF8, 0x00, 0x00, 0x00, 0x00,
    0x45, 0x47, 0x4D, 0x39, 0x36,
};

constexpr LatLongMslHeight kMslRecord = *LatLongMslHeight::parse(bytes(kMsl));

static_assert(kMslRecord.modelLength == kMsl.size() - LatLongMslHeight::kSize);
static_assert(kMslRecord.modelLength == 5);
static_assert(kMslRecord.model[0] == 'E' && kMslRecord.model[4] == '6');

// The fixed part alone is legal and names no model -- a receiver with no geoid
// applied is not an error.
static_assert(LatLongMslHeight::parse(std::span<const std::uint8_t>(kMsl).first(24)).has_value());
static_assert(
    LatLongMslHeight::parse(std::span<const std::uint8_t>(kMsl).first(24))->modelLength == 0);

// ---- Record 74: the epoch count that could not be placed -------------------
//
// The ICD gives this record 38 body bytes ending in a two-byte epoch count. A
// real receiver was seen to send 42, with a float where that count should be.
// Which four bytes moved could not be determined, so the count is reported only
// at exactly the documented length rather than guessed at.
constexpr std::array<std::uint8_t, 38> kSecondAntenna = [] {
    std::array<std::uint8_t, 38> out {};
    out[36] = 0x00;
    out[37] = 0x07;   // seven epochs
    return out;
}();

static_assert(SecondAntennaSigma::parse(bytes(kSecondAntenna))->hasEpochCount);
static_assert(SecondAntennaSigma::parse(bytes(kSecondAntenna))->epochCount == 7);

// One byte longer, and the count is ABSENT rather than read from where it used
// to be. This is the case the tree's usual "a longer record is accepted and its
// tail ignored" rule gets wrong, which is why this record does not follow it.
constexpr std::array<std::uint8_t, 42> kSecondAntennaLong = [] {
    std::array<std::uint8_t, 42> out {};
    out[36] = 0x42;
    out[37] = 0x83;   // a float, not a count -- 0x4283 would read as 17027 epochs
    return out;
}();

static_assert(SecondAntennaSigma::parse(bytes(kSecondAntennaLong)).has_value(),
              "the record still parses");
static_assert(!SecondAntennaSigma::parse(bytes(kSecondAntennaLong))->hasEpochCount,
              "but says it does not know the epoch count");
static_assert(SecondAntennaSigma::parse(bytes(kSecondAntennaLong))->epochCount == 0,
              "rather than reporting 17027 epochs for a one-epoch fix");

// The fields before the disputed tail are read either way.
static_assert(SecondAntennaSigma::parse(bytes(kSecondAntennaLong))->sigmaEastM == 0.0F);

// ---- Records 13, 14: the GPS-only lists and their strides ------------------
//
// Three bytes per satellite and eight, against records 33 and 34's four and
// ten. Getting a stride wrong reads every satellite after the first from the
// middle of its predecessor, which produces plausible PRNs and nonsense
// elevations rather than an error.
constexpr std::array<std::uint8_t, 7> kGpsBrief { 2, 16, 0x47, 0x00, 4, 0xCF, 0x04 };
constexpr SvBriefInfo kBrief13 = *SvBriefInfo::parse(bytes(kGpsBrief));

static_assert(kBrief13.count == 2);
static_assert(kGpsBrief.size() == SvBriefInfo::kSize + 2 * SvBriefInfo::kEntrySize);
static_assert(kBrief13.satellites[0].prn == 16 && kBrief13.satellites[1].prn == 4,
              "the second entry starts three bytes in, not four");

constexpr std::array<std::uint8_t, 17> kGpsDetail {
    2,
    16, 0x47, 0x00, 46, 0x00, 0x76, 0x57, 0x49,
    4,  0xCF, 0x04, 53, 0x01, 0x47, 0xA3, 0xAA,
};
constexpr SvDetailInfo kDetail14 = *SvDetailInfo::parse(bytes(kGpsDetail));

static_assert(kDetail14.count == 2);
static_assert(kGpsDetail.size() == SvDetailInfo::kSize + 2 * SvDetailInfo::kEntrySize);
static_assert(kDetail14.satellites[1].prn == 4, "the second entry starts eight bytes in");
static_assert(kDetail14.satellites[1].elevationDeg == 53);
static_assert(kDetail14.satellites[1].azimuthDeg == 327, "big-endian, so 0x0147 and not 0x4701");
static_assert(kDetail14.satellites[0].snrFirstScaled == 0x57);
static_assert(kDetail14.satellites[0].snrFirstDb() == 21.75F, "dB times four on the wire");

// ---- Records 92, 96: the empty case, which is the common one ---------------
//
// A receiver with no IonoGuard source reports 255 for both source and geofence
// and a satellite count of zero. That is not an error and not a truncated
// record: the fixed part is the whole record.
constexpr std::array<std::uint8_t, 10> kIono {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
};
static_assert(IonoGuardInfo::parse(bytes(kIono))->source() == IonoGuardSource::Invalid);
static_assert(IonoGuardInfo::parse(bytes(kIono))->count == 0);
static_assert(kIono.size() == IonoGuardInfo::kSize, "with no per-satellite block at all");

constexpr std::array<std::uint8_t, 13> kIonoWithSv {
    0x09, 0x81, 0x04, 0xEC, 0xE1, 0x28, 0x03, 0x00, 0x01, 0x01, 0x05, 0x14, 0x02,
};
constexpr IonoGuardInfo kIonoSv = *IonoGuardInfo::parse(bytes(kIonoWithSv));
static_assert(kIonoSv.source() == IonoGuardSource::Rtx);
static_assert(kIonoSv.count == 1);
static_assert(kIonoSv.satellites[0].prn == 20 && kIonoSv.satellites[0].system == 5);
static_assert(kIonoSv.satellites[0].level() == IonoGuardLevel::Orange);

constexpr std::array<std::uint8_t, 7> kIonoSummary { 0xFF, 0xFF, 0x00, 0x0C, 0x02, 0x01, 0x00 };
constexpr IonoGuardSummary kSummary = *IonoGuardSummary::parse(bytes(kIonoSummary));
static_assert(kSummary.source() == IonoGuardSource::Invalid);
static_assert(kSummary.greenSvs == 12 && kSummary.yellowSvs == 2 && kSummary.orangeSvs == 1);
static_assert(kSummary.redSvs == 0);

// ---- Record 62: a position record whose fields are offset by one -----------
//
// A position-type byte precedes the latitude, so every double in this record
// sits one byte later than in record 2. That single byte is the whole risk.
constexpr std::array<std::uint8_t, 43> kCode = [] {
    std::array<std::uint8_t, 43> out {};
    out[0] = 2;                                  // position type
    out[1] = 0x3F; out[2] = 0xE2;                // latitude starts at 1, not 0
    out[25] = 0x09; out[26] = 0x81;              // week 2433
    out[27] = 0x04; out[28] = 0xEC;              // time of week
    return out;
}();

constexpr CodePosition kCodeRecord = *CodePosition::parse(bytes(kCode));
static_assert(kCodeRecord.positionType == 2);
static_assert(kCodeRecord.gpsWeek == 2433, "the week is at 25, after three doubles and the type");
static_assert(kCodeRecord.latitudeRad != 0.0, "and the latitude began at byte 1");

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

// The lowest wire byte that is not in GSOF_RECORD_TABLE. The ICD's numbering
// is sparse, so one always exists.
std::uint8_t first_unmodelled_type()
{
    for (unsigned type = 1; type <= 255; ++type)
    {
        if (!is_known_record(static_cast<std::uint8_t>(type)))
        {
            return static_cast<std::uint8_t>(type);
        }
    }
    return 0;
}

void test_an_unknown_record_does_not_stop_the_walk()
{
    // THE case that matters. Enabling one unmodelled message on the receiver
    // must not silently disable every message the receiver sends after it.
    std::vector<std::uint8_t> payload;
    payload.push_back(1);
    payload.push_back(static_cast<std::uint8_t>(golden::kPositionTime.size()));
    payload.insert(payload.end(), golden::kPositionTime.begin(), golden::kPositionTime.end());

    // A type the table does not hold. Found rather than named, because this
    // test previously named record 92 -- which a live receiver then turned out
    // to send, so the table grew a row and the test went green for the wrong
    // reason. There is always an unmodelled type; asking is cheaper than
    // remembering.
    const std::uint8_t unmodelled = first_unmodelled_type();
    payload.push_back(unmodelled);
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

    check(seen == std::vector<std::uint8_t> { 1, unmodelled, 2 },
          "the record behind the unknown one is still found");
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
    check(std::string(record_name(RecordType::IonoGuardSummary)) == "ionoguard_summary", "records are named");
    check(is_known_record(27), "record 27 is modelled");
    check(is_known_record(92), "and so is record 92, since a live BD992 was seen to send it");
    check(!is_known_record(first_unmodelled_type()), "some type is not, by construction");
    check(!is_known_record(0), "and 0 never is");
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
