// SPDX-License-Identifier: GPL-3.0-or-later

#include "publishers.h"

#include <array>
#include <cmath>
#include <numbers>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "bd992.capnp.h"
#include "gsof_attitude.capnp.h"
#include "gsof_common.capnp.h"
#include "gsof_ins.capnp.h"
#include "gsof_position.capnp.h"
#include "gsof_satellites.capnp.h"
#include "gsof_status.capnp.h"
#include "pub_sub/zenoh_publisher.h"

namespace bd992_node
{

namespace
{

// The wire is radians in every record except 49 and 50. Consumers want
// degrees, and the conversion happens exactly here -- the library structs keep
// wire units so they still describe the bytes.
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

constexpr double toDegrees(double radians)
{
    return radians * kRadToDeg;
}

constexpr float toDegrees(float radians)
{
    return radians * static_cast<float>(kRadToDeg);
}

// A fixed-width, space-padded field on the wire, as Text without the padding.
template <std::size_t N>
std::string trimmed(const std::array<char, N>& field)
{
    std::string out(field.data(), N);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0'))
    {
        out.pop_back();
    }
    return out;
}

::GsofSvSystem svSystem(std::uint8_t raw)
{
    switch (static_cast<gsof::SvSystem>(raw))
    {
        case gsof::SvSystem::Gps:     return ::GsofSvSystem::GPS;
        case gsof::SvSystem::Sbas:    return ::GsofSvSystem::SBAS;
        case gsof::SvSystem::Glonass: return ::GsofSvSystem::GLONASS;
        case gsof::SvSystem::Galileo: return ::GsofSvSystem::GALILEO;
        case gsof::SvSystem::Qzss:    return ::GsofSvSystem::QZSS;
        case gsof::SvSystem::BeiDou:  return ::GsofSvSystem::BEIDOU;
    }

    // 6..255 are reserved in the ICD and a newer receiver will use them. The
    // raw byte travels alongside, so nothing is lost.
    return ::GsofSvSystem::UNKNOWN;
}

::GsofPositionFixType fixType(std::uint8_t raw)
{
    switch (static_cast<gsof::PositionFixType>(raw))
    {
        case gsof::PositionFixType::NoFixOrOld:             return ::GsofPositionFixType::NO_FIX_OR_OLD;
        case gsof::PositionFixType::Autonomous:             return ::GsofPositionFixType::AUTONOMOUS;
        case gsof::PositionFixType::PropagatedAutonomous:   return ::GsofPositionFixType::PROPAGATED_AUTONOMOUS;
        case gsof::PositionFixType::DifferentialSbas:       return ::GsofPositionFixType::DIFFERENTIAL_SBAS;
        case gsof::PositionFixType::PropagatedSbas:         return ::GsofPositionFixType::PROPAGATED_SBAS;
        case gsof::PositionFixType::Differential:           return ::GsofPositionFixType::DIFFERENTIAL;
        case gsof::PositionFixType::PropagatedDifferential: return ::GsofPositionFixType::PROPAGATED_DIFFERENTIAL;
        case gsof::PositionFixType::FloatRtk:               return ::GsofPositionFixType::FLOAT_RTK;
        case gsof::PositionFixType::PropagatedFloatRtk:     return ::GsofPositionFixType::PROPAGATED_FLOAT_RTK;
        case gsof::PositionFixType::FixedRtk:               return ::GsofPositionFixType::FIXED_RTK;
        case gsof::PositionFixType::PropagatedFixedRtk:     return ::GsofPositionFixType::PROPAGATED_FIXED_RTK;
        case gsof::PositionFixType::OmniStarHp:             return ::GsofPositionFixType::OMNI_STAR_HP;
        case gsof::PositionFixType::OmniStarXp:             return ::GsofPositionFixType::OMNI_STAR_XP;
        case gsof::PositionFixType::LocationRtk:            return ::GsofPositionFixType::LOCATION_RTK;
        case gsof::PositionFixType::OmniStarVbs:            return ::GsofPositionFixType::OMNI_STAR_VBS;
        case gsof::PositionFixType::BeaconDifferential:     return ::GsofPositionFixType::BEACON_DIFFERENTIAL;
        case gsof::PositionFixType::RtxCodePhase:           return ::GsofPositionFixType::RTX_CODE_PHASE;
        case gsof::PositionFixType::XFillRtx:               return ::GsofPositionFixType::X_FILL_RTX;
    }

    // The ICD's list runs to 48 with gaps and grows with firmware.
    // positionFixTypeRaw carries what actually arrived.
    return ::GsofPositionFixType::UNKNOWN;
}

void fillTime(::GsofGpsTime::Builder time, std::uint16_t week, std::uint32_t timeOfWeekMs)
{
    time.setWeek(week);
    time.setTimeOfWeekMs(timeOfWeekMs);
}

// ============================================================================
// One fill() per record. The compiler picks by overload, so a record without
// one is a build error rather than a topic that never appears.
// ============================================================================

void fill(::GsofPositionTime::Builder out, const gsof::PositionTime& in)
{
    fillTime(out.initTime(), in.gpsWeek, in.gpsTimeMs);
    out.setSvsUsed(in.svsUsed);
    out.setPositionFlags1(in.positionFlags1);
    out.setPositionFlags2(in.positionFlags2);
    out.setInitCounter(in.initCounter);

    out.setNewPosition(in.isNewPosition());
    out.setClockFix(in.hasClockFix());
    out.setHorizontalComputedHere(in.horizontalComputedHere());
    out.setHeightComputedHere(in.heightComputedHere());
    out.setWeightedLeastSquares(in.isWeightedLeastSquares());
    out.setUsedL1Pseudorange(in.usedL1Pseudorange());
    out.setDifferential(in.isDifferential());
    out.setDifferentialPhase(in.isDifferentialPhase());
    out.setFixedInteger(in.isFixedInteger());
    out.setOmniStar(in.isOmniStar());
    out.setStaticConstrained(in.isStaticConstrained());
    out.setNetworkRtk(in.isNetworkRtk());
    out.setLocationRtk(in.isLocationRtk());
    out.setBeaconDgps(in.isBeaconDgps());
}

void fill(::GsofLatLongHeight::Builder out, const gsof::LatLongHeight& in)
{
    out.setLatitudeDeg(toDegrees(in.latitudeRad));
    out.setLongitudeDeg(toDegrees(in.longitudeRad));
    out.setEllipsoidHeightM(in.heightM);
}

void fill(::GsofEcefPosition::Builder out, const gsof::EcefPosition& in)
{
    out.setXM(in.xM);
    out.setYM(in.yM);
    out.setZM(in.zM);
}

void fill(::GsofEcefDelta::Builder out, const gsof::EcefDelta& in)
{
    out.setDeltaXM(in.deltaXM);
    out.setDeltaYM(in.deltaYM);
    out.setDeltaZM(in.deltaZM);
}

void fill(::GsofTangentPlaneDelta::Builder out, const gsof::TangentPlaneDelta& in)
{
    out.setEastM(in.eastM);
    out.setNorthM(in.northM);
    out.setUpM(in.upM);
}

void fill(::GsofVelocity::Builder out, const gsof::Velocity& in)
{
    out.setVelocityFlags(in.velocityFlags);
    out.setValid(in.isValid());
    out.setDopplerDerived(in.isDopplerDerived());
    out.setHorizontalSpeedMps(in.horizontalSpeedMps);
    out.setHeadingDeg(toDegrees(in.headingRad));
    out.setVerticalVelocityMps(in.verticalVelocityMps);
    out.setHasLocalHeading(in.hasLocalHeading);
    out.setLocalHeadingDeg(toDegrees(in.localHeadingRad));
}

void fill(::GsofDopInfo::Builder out, const gsof::DopInfo& in)
{
    out.setPdop(in.pdop);
    out.setHdop(in.hdop);
    out.setVdop(in.vdop);
    out.setTdop(in.tdop);
}

void fill(::GsofClockInfo::Builder out, const gsof::ClockInfo& in)
{
    out.setClockFlags(in.clockFlags);
    out.setClockOffsetValid(in.isClockOffsetValid());
    out.setFrequencyOffsetValid(in.isFrequencyOffsetValid());
    out.setAnywhereFix(in.isReceiverInAnywhereFix());
    out.setReceiverClockOffsetMs(in.receiverClockOffsetMs);
    out.setFrequencyOffsetPpm(in.frequencyOffsetPpm);
}

void fill(::GsofPositionVcv::Builder out, const gsof::PositionVcv& in)
{
    out.setPositionRms(in.positionRms);
    out.setXx(in.xx);
    out.setXy(in.xy);
    out.setXz(in.xz);
    out.setYy(in.yy);
    out.setYz(in.yz);
    out.setZz(in.zz);
    out.setUnitVariance(in.unitVariance);
    out.setEpochCount(in.epochCount);
}

void fill(::GsofPositionSigma::Builder out, const gsof::PositionSigma& in)
{
    out.setPositionRms(in.positionRms);
    out.setSigmaEastM(in.sigmaEastM);
    out.setSigmaNorthM(in.sigmaNorthM);
    out.setCovarianceEastNorth(in.covarianceEastNorth);
    out.setSigmaUpM(in.sigmaUpM);
    out.setSemiMajorM(in.semiMajorM);
    out.setSemiMinorM(in.semiMinorM);
    // Already degrees on the wire, unlike everything else in this file.
    out.setOrientationDeg(in.orientationDeg);
    out.setUnitVariance(in.unitVariance);
    out.setEpochCount(in.epochCount);
}

void fill(::GsofReceiverSerial::Builder out, const gsof::ReceiverSerial& in)
{
    out.setSerialNumber(in.serialNumber);
}

void fill(::GsofCurrentTimeUtc::Builder out, const gsof::CurrentTimeUtc& in)
{
    fillTime(out.initTime(), in.gpsWeek, in.gpsTimeMs);
    out.setUtcOffsetS(in.utcOffsetS);
    out.setTimeValid(in.isTimeValid());
    out.setUtcOffsetValid(in.isUtcOffsetValid());
    out.setTimeFlags(in.timeFlags);
}

void fill(::GsofAttitudeInfo::Builder out, const gsof::AttitudeInfo& in)
{
    out.setGpsTimeMs(in.gpsTimeMs);
    out.setAttitudeFlags(in.attitudeFlags);
    out.setCalibrated(in.isCalibrated());
    out.setPitchValid(in.isPitchValid());
    out.setYawValid(in.isYawValid());
    out.setRollValid(in.isRollValid());
    out.setSvsUsed(in.svsUsed);
    out.setCalculationMode(in.calculationMode);

    out.setPitchDeg(toDegrees(in.pitchRad));
    out.setYawDeg(toDegrees(in.yawRad));
    out.setRollDeg(toDegrees(in.rollRad));

    out.setMasterSlaveRangeM(in.masterSlaveRangeM);
    out.setPdop(static_cast<float>(in.pdop()));

    out.setHasVariance(in.hasVariance);
    out.setPitchVariance(in.pitchVariance);
    out.setYawVariance(in.yawVariance);
    out.setRollVariance(in.rollVariance);
    out.setPitchYawCovariance(in.pitchYawCovariance);
    out.setPitchRollCovariance(in.pitchRollCovariance);
    out.setYawRollCovariance(in.yawRollCovariance);
    out.setMasterSlaveRangeVariance(in.masterSlaveRangeVariance);
}

void fill(::GsofAllSvBrief::Builder out, const gsof::AllSvBrief& in)
{
    out.setCount(in.count);

    ::capnp::List<::GsofSvBrief>::Builder list = out.initSatellites(in.count);
    for (std::size_t i = 0; i < in.count; ++i)
    {
        const gsof::SvBrief& sv = in.satellites[i];
        ::GsofSvBrief::Builder entry = list[static_cast<unsigned>(i)];
        entry.setPrn(sv.prn);
        entry.setSystem(svSystem(sv.system));
        entry.setSystemRaw(sv.system);
        entry.setFlags1(sv.flags.flags1);
        entry.setFlags2(sv.flags.flags2);
        entry.setAboveHorizon(sv.flags.aboveHorizon());
        entry.setUsedInPosition(sv.flags.usedInPosition());
        entry.setUsedInRtk(sv.flags.usedInRtk());
    }
}

void fill(::GsofAllSvDetailed::Builder out, const gsof::AllSvDetailed& in)
{
    out.setCount(in.count);

    ::capnp::List<::GsofSvDetail>::Builder list = out.initSatellites(in.count);
    for (std::size_t i = 0; i < in.count; ++i)
    {
        const gsof::SvDetail& sv = in.satellites[i];
        ::GsofSvDetail::Builder entry = list[static_cast<unsigned>(i)];
        entry.setPrn(sv.prn);
        entry.setSystem(svSystem(sv.system));
        entry.setSystemRaw(sv.system);
        entry.setFlags1(sv.flags.flags1);
        entry.setFlags2(sv.flags.flags2);
        entry.setAboveHorizon(sv.flags.aboveHorizon());
        entry.setUsedInPosition(sv.flags.usedInPosition());
        entry.setUsedInRtk(sv.flags.usedInRtk());
        entry.setElevationDeg(sv.elevationDeg);
        entry.setAzimuthDeg(sv.azimuthDeg);
        entry.setSnrFirstDb(sv.snrFirstDb());
        entry.setSnrSecondDb(sv.snrSecondDb());
        entry.setSnrThirdDb(sv.snrThirdDb());
    }
}

void fill(::GsofReceivedBase::Builder out, const gsof::ReceivedBase& in)
{
    out.setBaseFlags(in.baseFlags);
    out.setBaseInfoValid(in.isBaseInfoValid());
    out.setBaseName(trimmed(in.baseName));
    out.setBaseId(in.baseId);
    out.setLatitudeDeg(toDegrees(in.latitudeRad));
    out.setLongitudeDeg(toDegrees(in.longitudeRad));
    out.setEllipsoidHeightM(in.heightM);
}

void fill(::GsofBatteryMemory::Builder out, const gsof::BatteryMemory& in)
{
    out.setBatteryPercent(in.batteryPercent);
    out.setRemainingMemoryHours(in.remainingMemoryHours);
}

void fill(::GsofPositionType::Builder out, const gsof::PositionType& in)
{
    out.setPositionFixType(fixType(in.positionFixTypeRaw));
    out.setPositionFixTypeRaw(in.positionFixTypeRaw);
    out.setErrorScale(in.errorScale);
    out.setSolutionFlags(in.solutionFlags);
    out.setNetworkSolution(in.isNetworkSolution());
    out.setRtkFixed(in.isRtkFixed());
    out.setInitialisationIntegrity(in.initialisationIntegrity());
    out.setRtkCondition(in.rtkCondition);
    out.setCorrectionAgeS(in.correctionAgeS);
    out.setNetworkFlags(in.networkFlags);
    out.setNetworkFlags2(in.networkFlags2);
    out.setFrameFlag(in.frameFlag);
    out.setItrfEpochCentiYears(in.itrfEpochCentiYears);
    out.setTectonicPlate(in.tectonicPlate);
    out.setRtxSubscriptionMinutesLeft(in.rtxSubscriptionMinutesLeft);
    out.setPoleWobbleStatus(in.poleWobbleStatus);
    out.setPoleWobbleDistanceM(in.poleWobbleDistanceM);
}

void fill(::GsofLbandStatus::Builder out, const gsof::LbandStatus& in)
{
    out.setSatelliteName(trimmed(in.satelliteName));
    out.setNominalFrequencyMhz(in.nominalFrequencyMhz);
    out.setSatelliteBitRate(in.satelliteBitRate);
    out.setCnoDb(in.cnoDb);
    out.setHpxpSubscribedEngine(in.hpxpSubscribedEngine);
    out.setHpxpLibraryMode(in.hpxpLibraryMode);
    out.setVbsLibraryMode(in.vbsLibraryMode);
    out.setBeamMode(in.beamMode);
    out.setOmniStarMotion(in.omniStarMotion);
    out.setHorizontalSigmaThresholdM(in.horizontalSigmaThresholdM);
    out.setVerticalSigmaThresholdM(in.verticalSigmaThresholdM);
    out.setNmeaEncryptionState(in.nmeaEncryptionState);
    out.setIqRatio(in.iqRatio);
    out.setEstimatedBitErrorRate(in.estimatedBitErrorRate);
    out.setTotalMessages(in.totalMessages);
    out.setTotalUniqueWordsWithErrors(in.totalUniqueWordsWithErrors);
    out.setTotalBadUniqueWordBits(in.totalBadUniqueWordBits);
    out.setTotalViterbiSymbols(in.totalViterbiSymbols);
    out.setCorrectedViterbiSymbols(in.correctedViterbiSymbols);
    out.setBadMessages(in.badMessages);
    out.setMeasuredFrequencyValid(in.measuredFrequencyValid);
    out.setMeasuredFrequencyHz(in.measuredFrequencyHz);
}

void fill(::GsofBasePosition::Builder out, const gsof::BasePosition& in)
{
    fillTime(out.initTime(), in.gpsWeek, in.gpsTimeMs);
    out.setLatitudeDeg(toDegrees(in.latitudeRad));
    out.setLongitudeDeg(toDegrees(in.longitudeRad));
    out.setEllipsoidHeightM(in.heightM);
    out.setBaseQuality(in.baseQuality);
}

void fill(::GsofInsFullNav::Builder out, const gsof::InsFullNav& in)
{
    fillTime(out.initTime(), in.gpsWeek, in.gpsTimeMs);
    out.setImuAlignmentStatus(in.imuAlignmentStatus);
    out.setGnssStatus(in.gnssStatus);
    // Already degrees on the wire in this record.
    out.setLatitudeDeg(in.latitudeDeg);
    out.setLongitudeDeg(in.longitudeDeg);
    out.setAltitudeM(in.altitudeM);
    out.setVelocityNorthMps(in.velocityNorthMps);
    out.setVelocityEastMps(in.velocityEastMps);
    out.setVelocityDownMps(in.velocityDownMps);
    out.setTotalSpeedMps(in.totalSpeedMps);
    out.setRollDeg(in.rollDeg);
    out.setPitchDeg(in.pitchDeg);
    out.setHeadingDeg(in.headingDeg);
    out.setTrackAngleDeg(in.trackAngleDeg);
    out.setAngularRateRollDps(in.angularRateRollDps);
    out.setAngularRatePitchDps(in.angularRatePitchDps);
    out.setAngularRateHeadingDps(in.angularRateHeadingDps);
    out.setAccelerationXMps2(in.accelerationXMps2);
    out.setAccelerationYMps2(in.accelerationYMps2);
    out.setAccelerationZMps2(in.accelerationZMps2);
}

void fill(::GsofInsRms::Builder out, const gsof::InsRms& in)
{
    fillTime(out.initTime(), in.gpsWeek, in.gpsTimeMs);
    out.setImuAlignmentStatus(in.imuAlignmentStatus);
    out.setGnssStatus(in.gnssStatus);
    out.setPositionRmsNorthM(in.positionRmsNorthM);
    out.setPositionRmsEastM(in.positionRmsEastM);
    out.setPositionRmsDownM(in.positionRmsDownM);
    out.setVelocityRmsNorthMps(in.velocityRmsNorthMps);
    out.setVelocityRmsEastMps(in.velocityRmsEastMps);
    out.setVelocityRmsDownMps(in.velocityRmsDownMps);
    out.setRollRmsDeg(in.rollRmsDeg);
    out.setPitchRmsDeg(in.pitchRmsDeg);
    out.setHeadingRmsDeg(in.headingRmsDeg);
}

// The schema for a record, and where its publisher lives. Both generated, so
// they cannot disagree with the table or with each other.
template <typename RecordT>
struct SchemaOf;

} // namespace

// ============================================================================
// The publisher slots
// ============================================================================

struct Publishers::Impl
{
    std::string topicPrefix;
    bool publishUnknownRecords { false };

#define GSOF_PUB_SLOT(id, Name, snake) std::unique_ptr<pub_sub::ZenohPublisher<::Gsof##Name>> Name;
    GSOF_RECORD_TABLE(GSOF_PUB_SLOT)
#undef GSOF_PUB_SLOT

    std::unique_ptr<pub_sub::ZenohPublisher<::GsofRawRecord>> raw;

    struct Counter
    {
        std::string name;
        std::uint64_t count { 0 };
        std::chrono::steady_clock::time_point last {};
    };

    // Guards mCounters and mSerialNumber only. The reader thread writes them;
    // the main loop reads them to build the status message.
    mutable std::mutex mutex;
    std::unordered_map<std::uint8_t, Counter> counters;
    std::optional<std::int32_t> serialNumber;

    void note(std::uint8_t type, const char* name)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        Counter& counter = counters[type];
        if (counter.name.empty())
        {
            counter.name = name;
        }
        ++counter.count;
        counter.last = std::chrono::steady_clock::now();
    }
};

namespace
{

// SchemaOf maps a record struct to its schema and its publisher slot. `Gsof` +
// the table's name is the schema for every row, which is not a coincidence --
// the schemas were named to make this true.
#define GSOF_SCHEMA_TRAIT(id, Name, snake)                                             \
    template <>                                                                        \
    struct SchemaOf<gsof::Name>                                                         \
    {                                                                                  \
        using Schema = ::Gsof##Name;                                                   \
        static constexpr std::string_view topic = snake;                               \
        static std::unique_ptr<pub_sub::ZenohPublisher<Schema>>& slot(Publishers::Impl& impl) \
        {                                                                              \
            return impl.Name;                                                          \
        }                                                                              \
    };
GSOF_RECORD_TABLE(GSOF_SCHEMA_TRAIT)
#undef GSOF_SCHEMA_TRAIT

template <typename RecordT>
void publishOne(Publishers::Impl& impl, const RecordT& record)
{
    using Traits = SchemaOf<RecordT>;

    auto& slot = Traits::slot(impl);
    if (!slot)
    {
        // Created on first sight, so the topic's liveliness advertisement
        // names something the receiver is really sending.
        const std::string key = impl.topicPrefix + "/gsof/" + std::string(Traits::topic);
        slot = std::make_unique<pub_sub::ZenohPublisher<typename Traits::Schema>>(key);
        SPDLOG_INFO("bd992: publishing {} on {}", gsof::record_name(RecordT::kType), key);
    }

    fill(slot->fields(), record);
    slot->put();

    impl.note(static_cast<std::uint8_t>(RecordT::kType), gsof::record_name(RecordT::kType));
}

} // namespace

Publishers::Publishers(std::string topicPrefix, bool publishUnknownRecords) :
    mImpl(std::make_unique<Impl>())
{
    mImpl->topicPrefix = std::move(topicPrefix);
    mImpl->publishUnknownRecords = publishUnknownRecords;
}

Publishers::~Publishers() = default;

void Publishers::publish(const gsof::RawRecord& record)
{
    const gsof::Result<void> visited = gsof::visit_record(record, [this](const auto& parsed) {
        publishOne(*mImpl, parsed);

        // GSOF 15 is the only place the serial number appears on the stream,
        // and the receiver-info service would otherwise need a control round
        // trip to answer a question it has already been told the answer to.
        using Parsed = std::decay_t<decltype(parsed)>;
        if constexpr (std::is_same_v<Parsed, gsof::ReceiverSerial>)
        {
            const std::lock_guard<std::mutex> lock(mImpl->mutex);
            mImpl->serialNumber = parsed.serialNumber;
        }
    });

    if (visited.has_value())
    {
        return;
    }

    if (visited.error().kind != gsof::ErrorKind::UnknownRecord)
    {
        // A record we model whose body did not decode: a firmware change, or a
        // bug here. Not published, because a half-decoded position is worse
        // than none.
        SPDLOG_WARN("bd992: record {} did not decode: {}", record.type,
                    gsof::to_string(visited.error().kind));
        return;
    }

    if (!mImpl->publishUnknownRecords)
    {
        return;
    }

    if (!mImpl->raw)
    {
        const std::string key = mImpl->topicPrefix + "/gsof/raw";
        mImpl->raw = std::make_unique<pub_sub::ZenohPublisher<::GsofRawRecord>>(key);
        SPDLOG_INFO("bd992: publishing unmodelled records on {}", key);
    }

    ::GsofRawRecord::Builder out = mImpl->raw->fields();
    out.setRecordType(record.type);
    out.setBytes(::capnp::Data::Reader(record.body.data(), record.body.size()));
    mImpl->raw->put();

    mImpl->note(record.type, "raw");
}

std::vector<Publishers::Seen> Publishers::seen() const
{
    const auto now = std::chrono::steady_clock::now();

    const std::lock_guard<std::mutex> lock(mImpl->mutex);

    std::vector<Seen> out;
    out.reserve(mImpl->counters.size());

    for (const auto& [type, counter] : mImpl->counters)
    {
        out.push_back(Seen {
            type,
            counter.name,
            counter.count,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - counter.last).count()),
        });
    }

    return out;
}

std::optional<std::int32_t> Publishers::serialNumber() const
{
    const std::lock_guard<std::mutex> lock(mImpl->mutex);
    return mImpl->serialNumber;
}

} // namespace bd992_node
