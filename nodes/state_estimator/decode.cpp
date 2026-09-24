// SPDX-License-Identifier: GPL-3.0-or-later

#include "decode.h"

#include "gsof_attitude.capnp.h"
#include "gsof_common.capnp.h"
#include "gsof_position.capnp.h"
#include "gsof_status.capnp.h"
#include "pub_sub/capnp_payload.h"
#include "xbus_common.capnp.h"
#include "xbus_environment.capnp.h"
#include "xbus_inertial.capnp.h"

#include <capnp/serialize.h>

#include <algorithm>

namespace state_estimator
{

namespace
{

ImuHeader header(::XbusSampleHeader::Reader h)
{
    return ImuHeader{h.getPacketCounter(), h.getSampleTimeFineTicks()};
}

}  // namespace

vehicle_estimator::FixQuality fixQuality(GsofPositionFixType type)
{
    using Q = vehicle_estimator::FixQuality;
    using T = ::GsofPositionFixType;
    switch (type)
    {
        case T::UNKNOWN:
        case T::NO_FIX_OR_OLD:
            return Q::none;
        // Propagated solutions are the receiver coasting on an old fix; they
        // are worth no more than an autonomous one.
        case T::AUTONOMOUS:
        case T::PROPAGATED_AUTONOMOUS:
        case T::PROPAGATED_SBAS:
        case T::PROPAGATED_DIFFERENTIAL:
        case T::PROPAGATED_FLOAT_RTK:
        case T::PROPAGATED_FIXED_RTK:
        case T::INS_AUTONOMOUS:
        case T::INS_DEAD_RECKONING:
            return Q::autonomous;
        case T::DIFFERENTIAL_SBAS:
        case T::DIFFERENTIAL:
        case T::BEACON_DIFFERENTIAL:
        case T::OMNI_STAR_VBS:
        case T::OMNI_STAR_L1_ONLY:
        case T::L1S_SLAS:
        case T::INS_SBAS:
        case T::INS_CODE_PHASE_DGNSS:
        case T::RTX_CODE_PHASE:
        case T::INS_RTX_CODE_PHASE:
            return Q::differential;
        case T::FLOAT_RTK:
        case T::LOCATION_RTK:
            return Q::float_rtk;
        // Precise point positioning services: centimetres once converged.
        case T::OMNI_STAR_HP:
        case T::OMNI_STAR_XP:
        case T::X_FILL_RTX:
        case T::OMNI_STAR_HP_XP:
        case T::OMNI_STAR_HP_G2:
        case T::OMNI_STAR_G2:
        case T::SYNCHRONOUS_RTX:
        case T::LOW_LATENCY_RTX:
        case T::OMNI_STAR_MULTIPLE_SOURCE:
        case T::INS_RTX_CARRIER_PHASE:
        case T::INS_OMNI_STAR:
        case T::RTX_FAST_SYNC:
        case T::RTX_FAST_LOW_LATENCY:
        case T::LOW_LATENCY_RTX_RANGE_POINT:
        case T::SYNCHRONOUS_RTX_RANGE_POINT:
        case T::LOW_LATENCY_RTX_VIEW_POINT:
        case T::SYNCHRONOUS_RTX_VIEW_POINT:
        case T::LOW_LATENCY_RTX_FIELD_POINT:
        case T::SYNCHRONOUS_RTX_FIELD_POINT:
        case T::OMNI_STAR_G2_PLUS:
        case T::OMNI_STAR_G4_PLUS:
        case T::INS_X_FILL_RTX:
        case T::CLAS:
        case T::INS_CLAS:
        case T::HAS:
        case T::INS_HAS:
            return Q::rtx;
        case T::FIXED_RTK:
        case T::INS_RTK:
            return Q::fixed_rtk;
    }
    return Q::none;
}

Fed feed(std::string_view schema, std::span<const std::uint8_t> payload, double arrival, GnssAssembler& gnss,
         ImuAssembler& imu)
{
    static constexpr std::string_view kConsumed[] = {
        "GsofPositionTime", "GsofCurrentTimeUtc", "GsofLatLongHeight", "GsofVelocity", "GsofPositionSigma",
        "GsofAttitudeInfo", "GsofPositionType",   "XbusDeltaQ",        "XbusDeltaV",
        "XbusMagneticField", "XbusBaroPressure",
    };
    if (std::find(std::begin(kConsumed), std::end(kConsumed), schema) == std::end(kConsumed)) return Fed::ignored;

    const pub_sub::WordAlignedPayload aligned(kj::ArrayPtr<const kj::byte>(payload.data(), payload.size()));
    if (aligned.words().size() == 0) return Fed::malformed;
    try
    {
        ::capnp::FlatArrayMessageReader reader(aligned.words());
        if (schema == "GsofPositionTime")
        {
            const auto r = reader.getRoot<::GsofPositionTime>();
            gnss.addTime(GpsTimeRecord{r.getTime().getWeek(), r.getTime().getTimeOfWeekMs()}, arrival);
        }
        else if (schema == "GsofCurrentTimeUtc")
        {
            const auto r = reader.getRoot<::GsofCurrentTimeUtc>();
            // Only the week is taken from it: its time of week is the
            // receiver's clock, not a measurement epoch.
            if (r.getTimeValid()) gnss.addWeek(r.getTime().getWeek());
        }
        else if (schema == "GsofLatLongHeight")
        {
            const auto r = reader.getRoot<::GsofLatLongHeight>();
            gnss.addPosition(LatLongHeightRecord{r.getLatitudeDeg(), r.getLongitudeDeg(), r.getEllipsoidHeightM()},
                             arrival);
        }
        else if (schema == "GsofVelocity")
        {
            const auto r = reader.getRoot<::GsofVelocity>();
            gnss.addVelocity(VelocityRecord{r.getValid(), r.getHorizontalSpeedMps(), r.getHeadingDeg(),
                                            r.getVerticalVelocityMps()},
                             arrival);
        }
        else if (schema == "GsofPositionSigma")
        {
            const auto r = reader.getRoot<::GsofPositionSigma>();
            gnss.addSigma(SigmaRecord{r.getSigmaEastM(), r.getSigmaNorthM(), r.getCovarianceEastNorth(),
                                      r.getSigmaUpM()},
                          arrival);
        }
        else if (schema == "GsofAttitudeInfo")
        {
            const auto r = reader.getRoot<::GsofAttitudeInfo>();
            AttitudeRecord a;
            a.timeOfWeekMs = r.getGpsTimeMs();
            a.pitchValid = r.getPitchValid();
            a.yawValid = r.getYawValid();
            a.pitchDeg = r.getPitchDeg();
            a.yawDeg = r.getYawDeg();
            a.hasVariance = r.getHasVariance();
            a.pitchVariance = r.getPitchVariance();
            a.yawVariance = r.getYawVariance();
            a.pitchYawCovariance = r.getPitchYawCovariance();
            gnss.addAttitude(a, arrival);
        }
        else if (schema == "GsofPositionType")
        {
            gnss.addFix(fixQuality(reader.getRoot<::GsofPositionType>().getPositionFixType()), arrival);
        }
        else if (schema == "XbusDeltaQ")
        {
            const auto r = reader.getRoot<::XbusDeltaQ>();
            if (!r.getHeader().getHasPacketCounter() || !r.getHeader().getHasSampleTimeFine()) return Fed::malformed;
            imu.addDeltaQ(header(r.getHeader()),
                          Eigen::Quaterniond(r.getDeltaQW(), r.getDeltaQX(), r.getDeltaQY(), r.getDeltaQZ()), arrival);
        }
        else if (schema == "XbusDeltaV")
        {
            const auto r = reader.getRoot<::XbusDeltaV>();
            if (!r.getHeader().getHasPacketCounter() || !r.getHeader().getHasSampleTimeFine()) return Fed::malformed;
            imu.addDeltaV(header(r.getHeader()), Eigen::Vector3d(r.getDeltaVXMps(), r.getDeltaVYMps(), r.getDeltaVZMps()),
                          arrival);
        }
        else if (schema == "XbusMagneticField")
        {
            const auto r = reader.getRoot<::XbusMagneticField>();
            const auto h = r.getHeader();
            if (!h.getHasPacketCounter() || !h.getHasSampleTimeFine()) return Fed::malformed;
            // A clipped axis reads a plausible wrong field: the saturated
            // value, not the field. Not used at all.
            if (h.getHasStatus() && (h.getClipMagnetometerX() || h.getClipMagnetometerY() || h.getClipMagnetometerZ()))
                return Fed::ignored;
            const Eigen::Vector3d au(r.getMagneticFieldXAu(), r.getMagneticFieldYAu(), r.getMagneticFieldZAu());
            if (!au.allFinite()) return Fed::malformed;
            imu.addMagneticField(header(h), au);
        }
        else if (schema == "XbusBaroPressure")
        {
            const auto r = reader.getRoot<::XbusBaroPressure>();
            const auto h = r.getHeader();
            if (!h.getHasPacketCounter() || !h.getHasSampleTimeFine()) return Fed::malformed;
            // Zero is not a pressure anywhere a car drives; it is a sensor
            // that has not produced one.
            if (r.getPressurePa() == 0) return Fed::malformed;
            imu.addPressure(header(h), static_cast<double>(r.getPressurePa()));
        }
        return Fed::used;
    }
    catch (const kj::Exception&)
    {
        return Fed::malformed;
    }
}

}  // namespace state_estimator
