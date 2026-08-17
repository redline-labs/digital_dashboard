// SPDX-License-Identifier: GPL-3.0-or-later
#include "publishers.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <spdlog/spdlog.h>

#include "gsof_common.capnp.h"
#include "gsof_epoch.capnp.h"
#include "gsof_position.capnp.h"
#include "pub_sub/zenoh_publisher.h"

namespace bd992_mock
{
namespace
{

// Seconds between the Unix epoch (1970-01-01) and the GPS epoch (1980-01-06).
constexpr double kGpsEpochUnixS = 315964800.0;

// Leap seconds inserted since the GPS epoch. Fixed here because a mock has no
// receiver to ask; a real one reports it in GSOF record 16.
constexpr double kLeapSecondsS = 18.0;

constexpr double kSecondsPerWeek = 604800.0;

// What the simulated receiver claims about itself. Constants rather than
// configuration: they exist so the epoch's quality fields are internally
// consistent with an RTK-fixed solution, not so they can be tuned.
constexpr std::uint8_t kSvsUsed = 18;
constexpr std::uint8_t kFixedRtkRaw = 9; // gsof::PositionFixType::FixedRtk
constexpr float kCorrectionAgeS = 1.2f;
constexpr float kPositionRmsM = 0.020f;
constexpr float kSigmaHorizontalM = 0.014f;
constexpr float kSigmaUpM = 0.025f;

// GSOF 1 flag bytes for a fixed-integer differential-phase solution, computed
// here rather than at the receiver. Bit positions are gsof::PositionTime's.
constexpr std::uint8_t kPositionFlags1 = 0xEF; // new, clock fix, h+v here, L1, diff, diff phase
constexpr std::uint8_t kPositionFlags2 = 0x01; // fixed integer

// Metres per degree of latitude. Only used to scale the optional noise, where
// a percent is neither here nor there.
constexpr double kMetresPerDegreeLat = 111132.0;

} // namespace

GpsTime gpsTimeFromUnix(double unixSeconds)
{
    GpsTime out;

    const double gpsSeconds = (unixSeconds - kGpsEpochUnixS) + kLeapSecondsS;
    if (gpsSeconds < 0.0)
    {
        return out;
    }

    const double weeks = std::floor(gpsSeconds / kSecondsPerWeek);
    const double intoWeek = gpsSeconds - (weeks * kSecondsPerWeek);

    out.week = static_cast<std::uint16_t>(weeks);
    out.timeOfWeekMs = static_cast<std::uint32_t>(std::llround(intoWeek * 1000.0));
    return out;
}

struct Publishers::Impl
{
    std::unique_ptr<pub_sub::ZenohPublisher<::GsofEpoch>> epoch;
    std::unique_ptr<pub_sub::ZenohPublisher<::GsofPositionTime>> positionTime;
    std::unique_ptr<pub_sub::ZenohPublisher<::GsofLatLongHeight>> latLongHeight;
    std::unique_ptr<pub_sub::ZenohPublisher<::GsofVelocity>> velocity;
    std::unique_ptr<pub_sub::ZenohPublisher<::GsofPositionType>> positionType;
    std::unique_ptr<pub_sub::ZenohPublisher<::GsofPositionSigma>> positionSigma;
};

Publishers::Publishers(const PublishConfig& config) :
    mConfig(config), mImpl(std::make_unique<Impl>()), mNoise(config.noiseSeed)
{
    const std::string epochKey = mConfig.topicPrefix + "/epoch";
    mImpl->epoch = std::make_unique<pub_sub::ZenohPublisher<::GsofEpoch>>(epochKey);
    SPDLOG_INFO("[mock] publishing fused epochs on {}", epochKey);

    if (!mConfig.publishRecords)
    {
        return;
    }

    const std::string prefix = mConfig.topicPrefix + "/gsof/";
    mImpl->positionTime =
        std::make_unique<pub_sub::ZenohPublisher<::GsofPositionTime>>(prefix + "position_time");
    mImpl->latLongHeight =
        std::make_unique<pub_sub::ZenohPublisher<::GsofLatLongHeight>>(prefix + "lat_long_height");
    mImpl->velocity =
        std::make_unique<pub_sub::ZenohPublisher<::GsofVelocity>>(prefix + "velocity");
    mImpl->positionType =
        std::make_unique<pub_sub::ZenohPublisher<::GsofPositionType>>(prefix + "position_type");
    mImpl->positionSigma =
        std::make_unique<pub_sub::ZenohPublisher<::GsofPositionSigma>>(prefix + "position_sigma");
    SPDLOG_INFO("[mock] publishing 5 per-record topics under {}", prefix);
}

Publishers::~Publishers() = default;

void Publishers::publish(const VehicleState& state, const GpsTime& time)
{
    double latitudeDeg = state.latitudeDeg;
    double longitudeDeg = state.longitudeDeg;

    if (mConfig.positionNoiseM > 0.0)
    {
        std::normal_distribution<double> metres(0.0, mConfig.positionNoiseM);
        const double rad = std::numbers::pi / 180.0;
        const double cosLat = std::max(std::cos(latitudeDeg * rad), 1e-6);
        latitudeDeg += metres(mNoise) / kMetresPerDegreeLat;
        longitudeDeg += metres(mNoise) / (kMetresPerDegreeLat * cosLat);
    }

    ++mSequence;

    // ---- The fused epoch ---------------------------------------------------
    //
    // EVERY FIELD IS SET EVERY TICK. put() re-roots the builder, so a field
    // left alone is not "unchanged from last time", it is zero -- and a
    // hasVelocity of false reads downstream as a receiver that stopped sending
    // record 8.
    {
        ::GsofEpoch::Builder out = mImpl->epoch->fields();
        out.setSequence(mSequence);

        out.setLatitudeDeg(latitudeDeg);
        out.setLongitudeDeg(longitudeDeg);
        out.setEllipsoidHeightM(mConfig.ellipsoidHeightM);

        out.setHasTime(true);
        ::GsofGpsTime::Builder stamp = out.initTime();
        stamp.setWeek(time.week);
        stamp.setTimeOfWeekMs(time.timeOfWeekMs);
        out.setSvsUsed(kSvsUsed);

        out.setHasVelocity(true);
        // Valid even at a standstill: a stopped vehicle has a speed, and it is
        // zero. map_match already declines to trust a heading below
        // heading_valid_above_mps, which is the right place for that judgement.
        out.setVelocityValid(true);
        out.setDopplerDerived(true);
        out.setHorizontalSpeedMps(static_cast<float>(state.speedMps));
        out.setHeadingDeg(static_cast<float>(state.headingDeg));
        out.setVerticalVelocityMps(0.0f);

        out.setHasFixType(true);
        out.setPositionFixType(::GsofPositionFixType::FIXED_RTK);
        out.setPositionFixTypeRaw(kFixedRtkRaw);
        out.setRtkFixed(true);
        out.setCorrectionAgeS(kCorrectionAgeS);

        out.setHasSigma(true);
        out.setPositionRmsM(kPositionRmsM);
        out.setSigmaEastM(kSigmaHorizontalM);
        out.setSigmaNorthM(kSigmaHorizontalM);
        out.setSigmaUpM(kSigmaUpM);

        mImpl->epoch->put();
    }

    if (!mConfig.publishRecords)
    {
        return;
    }

    // ---- The per-record topics --------------------------------------------

    {
        ::GsofPositionTime::Builder out = mImpl->positionTime->fields();
        ::GsofGpsTime::Builder stamp = out.initTime();
        stamp.setWeek(time.week);
        stamp.setTimeOfWeekMs(time.timeOfWeekMs);
        out.setSvsUsed(kSvsUsed);
        out.setPositionFlags1(kPositionFlags1);
        out.setPositionFlags2(kPositionFlags2);
        out.setInitCounter(0);
        // The decoded bits, which must agree with the two bytes above.
        out.setNewPosition(true);
        out.setClockFix(true);
        out.setHorizontalComputedHere(true);
        out.setHeightComputedHere(true);
        out.setWeightedLeastSquares(false);
        out.setUsedL1Pseudorange(true);
        out.setDifferential(true);
        out.setDifferentialPhase(true);
        out.setFixedInteger(true);
        out.setOmniStar(false);
        out.setStaticConstrained(false);
        out.setNetworkRtk(false);
        out.setLocationRtk(false);
        out.setBeaconDgps(false);
        mImpl->positionTime->put();
    }

    {
        ::GsofLatLongHeight::Builder out = mImpl->latLongHeight->fields();
        out.setLatitudeDeg(latitudeDeg);
        out.setLongitudeDeg(longitudeDeg);
        out.setEllipsoidHeightM(mConfig.ellipsoidHeightM);
        mImpl->latLongHeight->put();
    }

    {
        ::GsofVelocity::Builder out = mImpl->velocity->fields();
        // Bit 0 valid, bit 1 Doppler-derived.
        out.setVelocityFlags(0x03);
        out.setValid(true);
        out.setDopplerDerived(true);
        out.setHorizontalSpeedMps(static_cast<float>(state.speedMps));
        out.setHeadingDeg(static_cast<float>(state.headingDeg));
        out.setVerticalVelocityMps(0.0f);
        // The local-heading field is present only on the longer form of record
        // 8, and this receiver does not send it.
        out.setHasLocalHeading(false);
        out.setLocalHeadingDeg(0.0f);
        mImpl->velocity->put();
    }

    {
        ::GsofPositionType::Builder out = mImpl->positionType->fields();
        out.setPositionFixType(::GsofPositionFixType::FIXED_RTK);
        out.setPositionFixTypeRaw(kFixedRtkRaw);
        out.setErrorScale(1.0f);
        out.setSolutionFlags(0x00);
        out.setNetworkSolution(false);
        out.setRtkFixed(true);
        out.setInitialisationIntegrity(0);
        out.setRtkCondition(0);
        out.setCorrectionAgeS(kCorrectionAgeS);
        out.setNetworkFlags(0);
        out.setNetworkFlags2(0);
        out.setFrameFlag(0);
        out.setItrfEpochCentiYears(0);
        out.setTectonicPlate(0);
        out.setRtxSubscriptionMinutesLeft(0);
        out.setPoleWobbleStatus(0);
        out.setPoleWobbleDistanceM(0.0f);
        mImpl->positionType->put();
    }

    {
        ::GsofPositionSigma::Builder out = mImpl->positionSigma->fields();
        out.setPositionRms(kPositionRmsM);
        out.setSigmaEastM(kSigmaHorizontalM);
        out.setSigmaNorthM(kSigmaHorizontalM);
        out.setCovarianceEastNorth(0.0f);
        out.setSigmaUpM(kSigmaUpM);
        out.setSemiMajorM(kSigmaHorizontalM);
        out.setSemiMinorM(kSigmaHorizontalM);
        out.setOrientationDeg(0.0f);
        out.setUnitVariance(1.0f);
        out.setEpochCount(1);
        mImpl->positionSigma->put();
    }
}

} // namespace bd992_mock
