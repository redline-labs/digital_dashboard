// SPDX-License-Identifier: GPL-3.0-or-later

#include "sim_bus.h"

#include "geodesy/geodetic.h"
#include "gsof_attitude.capnp.h"
#include "gsof_common.capnp.h"
#include "gsof_position.capnp.h"
#include "xbus_common.capnp.h"
#include "xbus_environment.capnp.h"
#include "xbus_inertial.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cmath>
#include <numbers>

namespace state_estimator
{

namespace
{

constexpr double kToDeg = 180.0 / std::numbers::pi;

template <typename Schema, typename Fill>
BusMessage encode(std::string key, const char* schema, double arrival, Fill fill)
{
    ::capnp::MallocMessageBuilder builder;
    fill(builder.initRoot<Schema>());
    const kj::Array<::capnp::word> words = ::capnp::messageToFlatArray(builder);
    const auto bytes = words.asBytes();
    return BusMessage{std::move(key), schema, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), arrival};
}

void header(::XbusSampleHeader::Builder h, const vehicle_estimator::ImuSample& s)
{
    h.setHasPacketCounter(true);
    h.setPacketCounter(s.packet_counter);
    h.setHasSampleTimeFine(true);
    h.setSampleTimeFineTicks(s.sample_time_fine);
    h.setPrecision(::XbusPrecision::FP1632);
}

::GsofPositionFixType fixType(vehicle_estimator::FixQuality f)
{
    using Q = vehicle_estimator::FixQuality;
    switch (f)
    {
        case Q::none:
            return ::GsofPositionFixType::NO_FIX_OR_OLD;
        case Q::autonomous:
            return ::GsofPositionFixType::AUTONOMOUS;
        case Q::differential:
            return ::GsofPositionFixType::DIFFERENTIAL;
        case Q::float_rtk:
            return ::GsofPositionFixType::FLOAT_RTK;
        case Q::rtx:
            return ::GsofPositionFixType::SYNCHRONOUS_RTX;
        case Q::fixed_rtk:
            return ::GsofPositionFixType::FIXED_RTK;
    }
    return ::GsofPositionFixType::UNKNOWN;
}

}  // namespace

std::vector<BusMessage> toBus(const vehicle_estimator::sim::Scenario& scenario, const BusLayout& layout)
{
    std::vector<BusMessage> out;
    const std::string imu = layout.imuPrefix + "/", gsof = layout.gnssPrefix + "/";
    unsigned epoch = 0;
    for (const auto& m : scenario.messages())
    {
        if (m.imu)
        {
            const auto& s = *m.imu;
            out.push_back(encode<::XbusDeltaQ>(imu + "delta_q", "XbusDeltaQ", s.host_time, [&](auto b) {
                header(b.initHeader(), s);
                b.setDeltaQW(s.dq.w());
                b.setDeltaQX(s.dq.x());
                b.setDeltaQY(s.dq.y());
                b.setDeltaQZ(s.dq.z());
            }));
            out.push_back(encode<::XbusDeltaV>(imu + "delta_v", "XbusDeltaV", s.host_time + 2e-6, [&](auto b) {
                header(b.initHeader(), s);
                b.setDeltaVXMps(s.dv.x());
                b.setDeltaVYMps(s.dv.y());
                b.setDeltaVZMps(s.dv.z());
            }));
            // The same packet's other items, with the same header, as the
            // bridge publishes them.
            if (s.mag_au)
            {
                out.push_back(encode<::XbusMagneticField>(imu + "magnetic_field", "XbusMagneticField", s.host_time + 4e-6,
                                                          [&](auto b) {
                                                              header(b.initHeader(), s);
                                                              b.setMagneticFieldXAu(s.mag_au->x());
                                                              b.setMagneticFieldYAu(s.mag_au->y());
                                                              b.setMagneticFieldZAu(s.mag_au->z());
                                                          }));
            }
            if (s.pressure_pa)
            {
                out.push_back(encode<::XbusBaroPressure>(imu + "baro_pressure", "XbusBaroPressure", s.host_time + 6e-6,
                                                         [&](auto b) {
                                                             header(b.initHeader(), s);
                                                             b.setPressurePa(static_cast<std::uint32_t>(*s.pressure_pa));
                                                         }));
            }
            continue;
        }
        const auto& e = *m.gnss;
        const double week = std::floor(e.gps_time / 604800.0);
        const auto tow_ms = static_cast<std::uint32_t>(std::llround((e.gps_time - week * 604800.0) * 1000.0));
        const auto wk = static_cast<std::uint16_t>(week);
        double t = e.host_time;
        const auto next = [&] { return t += 2e-5; };  // one transmission, records microseconds apart

        out.push_back(encode<::GsofPositionTime>(gsof + "position_time", "GsofPositionTime", next(), [&](auto b) {
            b.initTime().setWeek(wk);
            b.getTime().setTimeOfWeekMs(tow_ms);
        }));
        if (e.position)
        {
            out.push_back(encode<::GsofLatLongHeight>(gsof + "lat_long_height", "GsofLatLongHeight", next(), [&](auto b) {
                b.setLatitudeDeg(e.position->lat * kToDeg);
                b.setLongitudeDeg(e.position->lon * kToDeg);
                b.setEllipsoidHeightM(e.position->h);
            }));
        }
        if (e.velocity)
        {
            const Eigen::Vector3d& v = e.velocity->v_ned;
            out.push_back(encode<::GsofVelocity>(gsof + "velocity", "GsofVelocity", next(), [&](auto b) {
                b.setValid(true);
                b.setDopplerDerived(true);
                b.setHorizontalSpeedMps(static_cast<float>(std::hypot(v.x(), v.y())));
                b.setHeadingDeg(static_cast<float>(std::atan2(v.y(), v.x()) * kToDeg));
                b.setVerticalVelocityMps(static_cast<float>(-v.z()));
            }));
        }
        if (e.attitude)
        {
            out.push_back(encode<::GsofAttitudeInfo>(gsof + "attitude_info", "GsofAttitudeInfo", next(), [&](auto b) {
                b.setGpsTimeMs(tow_ms);
                b.setCalibrated(true);
                b.setPitchValid(true);
                b.setYawValid(true);
                b.setPitchDeg(e.attitude->pitch * kToDeg);
                b.setYawDeg(e.attitude->yaw * kToDeg);
                if (e.attitude->cov)
                {
                    b.setHasVariance(true);
                    b.setYawVariance(static_cast<float>((*e.attitude->cov)(0, 0)));
                    b.setPitchVariance(static_cast<float>((*e.attitude->cov)(1, 1)));
                    b.setPitchYawCovariance(static_cast<float>((*e.attitude->cov)(0, 1)));
                }
            }));
        }
        if (epoch++ % layout.slowEvery == 0)
        {
            if (e.position)
            {
                const Eigen::Matrix3d& c = e.position->cov_ned;
                out.push_back(encode<::GsofPositionSigma>(gsof + "position_sigma", "GsofPositionSigma", next(), [&](auto b) {
                    b.setSigmaNorthM(static_cast<float>(std::sqrt(c(0, 0))));
                    b.setSigmaEastM(static_cast<float>(std::sqrt(c(1, 1))));
                    b.setCovarianceEastNorth(static_cast<float>(c(0, 1)));
                    b.setSigmaUpM(static_cast<float>(std::sqrt(c(2, 2))));
                }));
            }
            out.push_back(encode<::GsofPositionType>(gsof + "position_type", "GsofPositionType", next(),
                                                     [&](auto b) { b.setPositionFixType(fixType(e.fix)); }));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const BusMessage& a, const BusMessage& b) { return a.arrival < b.arrival; });
    return out;
}

vehicle_estimator::VehicleState truthState(const vehicle_estimator::sim::Scenario& scenario, double t)
{
    const auto tr = scenario.truth(t);
    vehicle_estimator::VehicleState s;
    s.gps_time = scenario.sensors().gps_epoch + t;
    s.valid = true;
    const auto llh = geodesy::ecefToLlh(csym::Vector3<double>{tr.p_e.x(), tr.p_e.y(), tr.p_e.z()});
    s.lat = llh.lat;
    s.lon = llh.lon;
    s.h = llh.h;
    s.p_e = tr.p_e;
    s.v_ned = tr.v_ned;
    s.v_body = tr.v_body;
    s.rate_body = tr.rate_body;
    s.q_n_b = Eigen::Quaterniond(tr.R_n_b);
    s.roll = tr.roll;
    s.pitch = tr.pitch;
    s.yaw = tr.yaw;
    s.sideslip_valid = std::hypot(tr.v_body.x(), tr.v_body.y()) > 2.0;
    s.sideslip = s.sideslip_valid ? tr.sideslip : 0.0;
    s.fix = scenario.sensors().fix;
    return s;
}

}  // namespace state_estimator
