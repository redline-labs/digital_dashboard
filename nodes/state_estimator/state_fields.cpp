// SPDX-License-Identifier: GPL-3.0-or-later

#include "state_fields.h"

#include <numbers>

namespace state_estimator
{

namespace
{

constexpr double kToDeg = 180.0 / std::numbers::pi;

::VehicleFixQuality toWire(vehicle_estimator::FixQuality f)
{
    using Q = vehicle_estimator::FixQuality;
    switch (f)
    {
        case Q::none:
            return ::VehicleFixQuality::NONE;
        case Q::autonomous:
            return ::VehicleFixQuality::AUTONOMOUS;
        case Q::differential:
            return ::VehicleFixQuality::DIFFERENTIAL;
        case Q::float_rtk:
            return ::VehicleFixQuality::FLOAT_RTK;
        case Q::rtx:
            return ::VehicleFixQuality::RTX;
        case Q::fixed_rtk:
            return ::VehicleFixQuality::FIXED_RTK;
    }
    return ::VehicleFixQuality::NONE;
}

::VehicleHeadingSource toWire(vehicle_estimator::HeadingSource h)
{
    using H = vehicle_estimator::HeadingSource;
    switch (h)
    {
        case H::none:
            return ::VehicleHeadingSource::NONE;
        case H::dual_antenna:
            return ::VehicleHeadingSource::DUAL_ANTENNA;
        case H::magnetometer:
            return ::VehicleHeadingSource::MAGNETOMETER;
        case H::inertial:
            return ::VehicleHeadingSource::INERTIAL;
    }
    return ::VehicleHeadingSource::NONE;
}

std::uint32_t clampU32(std::size_t v)
{
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v);
}

}  // namespace

void fill(::VehicleState::Builder out, const vehicle_estimator::VehicleState& s)
{
    out.setGpsTimeS(s.gps_time);
    out.setValid(s.valid);
    out.setLatitudeDeg(s.lat * kToDeg);
    out.setLongitudeDeg(s.lon * kToDeg);
    out.setEllipsoidHeightM(s.h);
    out.setRollDeg(s.roll * kToDeg);
    out.setPitchDeg(s.pitch * kToDeg);
    out.setYawDeg(s.yaw * kToDeg);
    out.setVelocityNorthMps(s.v_ned.x());
    out.setVelocityEastMps(s.v_ned.y());
    out.setVelocityDownMps(s.v_ned.z());
    out.setVelocityForwardMps(s.v_body.x());
    out.setVelocityRightMps(s.v_body.y());
    out.setVelocityDownBodyMps(s.v_body.z());
    out.setSpeedMps(s.v_body.norm());
    out.setAccelForwardMps2(s.accel_body.x());
    out.setAccelRightMps2(s.accel_body.y());
    out.setAccelDownMps2(s.accel_body.z());
    out.setRollRateDps(s.rate_body.x() * kToDeg);
    out.setPitchRateDps(s.rate_body.y() * kToDeg);
    out.setYawRateDps(s.rate_body.z() * kToDeg);
    out.setSideslipDeg(s.sideslip * kToDeg);
    out.setSideslipValid(s.sideslip_valid);
    out.setSigmaNorthM(s.sigma_position_ned.x());
    out.setSigmaEastM(s.sigma_position_ned.y());
    out.setSigmaDownM(s.sigma_position_ned.z());
    out.setSigmaVelocityMps(s.sigma_velocity_ned.norm());
    out.setSigmaRollDeg(s.sigma_attitude.x() * kToDeg);
    out.setSigmaPitchDeg(s.sigma_attitude.y() * kToDeg);
    out.setSigmaYawDeg(s.sigma_attitude.z() * kToDeg);
    out.setSigmaSideslipDeg(s.sigma_sideslip * kToDeg);
    out.setFix(toWire(s.fix));
    out.setGpsTimeValid(s.gps_time_valid);
    out.setAttitudeValid(s.attitude_valid);
    out.setHeadingMagnetic(s.heading_magnetic);
    out.setHeadingSource(toWire(s.heading_source));
}

void fill(::VehicleEstimatorStatus::Builder out, const vehicle_estimator::EstimatorStatus& s,
          const CalibrationReport& c)
{
    out.setInitialized(s.initialized);
    out.setKeyframes(s.keyframes);
    out.setResets(s.resets);
    out.setImuSamples(s.imu_samples);
    out.setImuBridged(s.imu_bridged);
    out.setImuDiscarded(s.imu_discarded);
    out.setImuRestarts(s.imu_restarts);
    out.setGnssEpochs(s.gnss_epochs);
    out.setGnssLate(s.gnss_late);
    out.setGnssTimedOut(s.gnss_timed_out);
    out.setGnssRejected(s.gnss_rejected);
    out.setGatedPosition(s.gated_position);
    out.setGatedVelocity(s.gated_velocity);
    out.setGatedAttitude(s.gated_attitude);
    out.setUpdatesRefused(s.updates_refused);
    out.setSolveMs(s.last_solve_ms);
    out.setIterations(static_cast<std::uint32_t>(std::max(0, s.last_optimize.iterations)));
    out.setWindowVariables(clampU32(s.window_variables));
    out.setWindowFactors(clampU32(s.window_factors));
    out.setLeverArmXM(s.lever_arm.x());
    out.setLeverArmYM(s.lever_arm.y());
    out.setLeverArmZM(s.lever_arm.z());
    out.setLeverArmSigmaM(s.lever_arm_sigma.norm());
    out.setBoresight1Rad(s.boresight.x());
    out.setBoresight2Rad(s.boresight.y());
    out.setGyroBiasXDps(s.gyro_bias.x() * kToDeg);
    out.setGyroBiasYDps(s.gyro_bias.y() * kToDeg);
    out.setGyroBiasZDps(s.gyro_bias.z() * kToDeg);
    out.setAccelBiasXMps2(s.accel_bias.x());
    out.setAccelBiasYMps2(s.accel_bias.y());
    out.setAccelBiasZMps2(s.accel_bias.z());
    out.setHasImuClockOffset(s.imu_clock_offset.has_value());
    out.setImuClockOffsetS(s.imu_clock_offset.value_or(0.0));
    out.setMountingRollDeg(s.mounting_rpy.x() * kToDeg);
    out.setMountingPitchDeg(s.mounting_rpy.y() * kToDeg);
    out.setMountingYawDeg(s.mounting_rpy.z() * kToDeg);
    out.setMountingSigmaRollDeg(s.mounting_sigma.x() * kToDeg);
    out.setMountingSigmaPitchDeg(s.mounting_sigma.y() * kToDeg);
    out.setMountingSigmaYawDeg(s.mounting_sigma.z() * kToDeg);
    out.setMountingStraightS(s.mount_straight_s);
    out.setMountingLevelStops(clampU32(s.mount_level_stops));
    out.setCalibrationStoreOk(c.store_open);
    out.setMountingFromDatabase(c.from_database[0]);
    out.setLeverArmFromDatabase(c.from_database[1]);
    out.setBoresightFromDatabase(c.from_database[2]);
    out.setMountingMoved(c.moved[0]);
    out.setLeverArmMoved(c.moved[1]);
    out.setBoresightMoved(c.moved[2]);
    out.setCalibrationRowsWritten(c.rows_written);
    out.setCalibrationStoreRecovered(!c.store_recovered.empty());
    out.setAnchored(s.anchored);
    out.setReanchors(s.reanchors);
    out.setInertialKeyframes(s.inertial_keyframes);
    out.setZeroVelocityUpdates(s.zero_velocity_updates);
    out.setMagUsed(s.mag_used);
    out.setMagRejected(s.mag_rejected);
    out.setMagLearningS(s.mag_learning_s);
    out.setMagTrusted(s.mag_trusted);
    out.setMagHardIronX(s.mag_hard_iron.x());
    out.setMagHardIronY(s.mag_hard_iron.y());
    out.setMagHardIronZ(s.mag_hard_iron.z());
    out.setMagHardIronSigmaX(s.mag_hard_iron_sigma.x());
    out.setMagHardIronSigmaY(s.mag_hard_iron_sigma.y());
    out.setMagHardIronSigmaZ(s.mag_hard_iron_sigma.z());
    out.setMagSoftIronXx(s.mag_soft_iron[0]);
    out.setMagSoftIronYy(s.mag_soft_iron[1]);
    out.setMagSoftIronZz(s.mag_soft_iron[2]);
    out.setMagSoftIronXy(s.mag_soft_iron[3]);
    out.setMagSoftIronXz(s.mag_soft_iron[4]);
    out.setMagSoftIronYz(s.mag_soft_iron[5]);
    out.setBaroFactors(s.baro_factors);
    out.setBaroMovingS(s.baro_moving_s);
    out.setBaroHeightM(s.baro_height);
    out.setBaroOffsetM(s.baro_offset);
    out.setBaroOffsetSigmaM(s.baro_offset_sigma);
    out.setBaroAirflow(s.baro_airflow);
    out.setBaroAirflowSigma(s.baro_airflow_sigma);
    out.setMagnetometerFromDatabase(c.from_database[3]);
    out.setBaroAirflowFromDatabase(c.from_database[4]);
    out.setMagnetometerMoved(c.moved[3]);
    out.setBaroAirflowMoved(c.moved[4]);
}

}  // namespace state_estimator
