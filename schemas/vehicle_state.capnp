@0xb8f8ceb5b651b757;

# The state estimator's output: where the car is, which way it points, how it
# is moving, and how far it is sliding. Published by nodes/state_estimator at
# the IMU's rate.
#
# Every field is a flat scalar so a scope panel or a dashboard gauge can bind
# it directly. Frames: n is the local NED frame at the reference point; b is
# the vehicle body, SAE J670 -- x forward, y right, z down -- so a positive
# yaw rate is a right turn and a positive sideslip is the velocity pointing
# right of the nose.

enum VehicleFixQuality {
  none @0;
  autonomous @1;
  differential @2;
  floatRtk @3;
  rtx @4;
  fixedRtk @5;
}

struct VehicleState {
  # GPS time of the estimate: the whole seconds since the GPS epoch plus the
  # fraction, as one number -- week * 604800 + time of week.
  gpsTimeS @0 :Float64;

  # Initialised, and inside the configured uncertainty bounds. A consumer that
  # ignores this will eventually draw a car at the centre of the earth.
  valid @1 :Bool;

  # The reference point (configured: the CG, or the rear axle).
  latitudeDeg @2 :Float64;
  longitudeDeg @3 :Float64;
  ellipsoidHeightM @4 :Float64;

  rollDeg @5 :Float64;
  pitchDeg @6 :Float64;
  yawDeg @7 :Float64;

  velocityNorthMps @8 :Float64;
  velocityEastMps @9 :Float64;
  velocityDownMps @10 :Float64;

  # Velocity of the reference point in the body frame.
  velocityForwardMps @11 :Float64;
  velocityRightMps @12 :Float64;
  velocityDownBodyMps @13 :Float64;
  speedMps @14 :Float64;

  # Kinematic acceleration of the reference point, gravity removed, in the
  # body frame; the angular-acceleration term is not included.
  accelForwardMps2 @15 :Float64;
  accelRightMps2 @16 :Float64;
  accelDownMps2 @17 :Float64;

  # Angular rate relative to the earth, body frame.
  rollRateDps @18 :Float64;
  pitchRateDps @19 :Float64;
  yawRateDps @20 :Float64;

  # The angle of the reference point's velocity from the body x axis,
  # positive to the right. Undefined, and flagged so, below a configured speed.
  sideslipDeg @21 :Float64;
  sideslipValid @22 :Bool;

  # One sigma.
  sigmaNorthM @23 :Float64;
  sigmaEastM @24 :Float64;
  sigmaDownM @25 :Float64;
  sigmaVelocityMps @26 :Float64;
  sigmaRollDeg @27 :Float64;
  sigmaPitchDeg @28 :Float64;
  sigmaYawDeg @29 :Float64;
  sigmaSideslipDeg @30 :Float64;

  fix @31 :VehicleFixQuality;
}

# Once a second: what the estimator is doing and what it has learned about
# the car.
struct VehicleEstimatorStatus {
  initialized @0 :Bool;
  keyframes @1 :UInt64;
  resets @2 :UInt64;

  imuSamples @3 :UInt64;
  imuBridged @4 :UInt64;
  imuDiscarded @5 :UInt64;
  imuRestarts @6 :UInt64;

  gnssEpochs @7 :UInt64;
  gnssLate @8 :UInt64;
  gnssTimedOut @9 :UInt64;
  gnssRejected @10 :UInt64;
  gatedPosition @11 :UInt64;
  gatedVelocity @12 :UInt64;
  gatedAttitude @13 :UInt64;
  updatesRefused @14 :UInt64;

  # The last smoother update.
  solveMs @15 :Float64;
  iterations @16 :UInt32;
  windowVariables @17 :UInt32;
  windowFactors @18 :UInt32;

  # Calibration as currently estimated, IMU frame.
  leverArmXM @19 :Float64;
  leverArmYM @20 :Float64;
  leverArmZM @21 :Float64;
  leverArmSigmaM @22 :Float64;
  boresight1Rad @23 :Float64;
  boresight2Rad @24 :Float64;
  gyroBiasXDps @25 :Float64;
  gyroBiasYDps @26 :Float64;
  gyroBiasZDps @27 :Float64;
  accelBiasXMps2 @28 :Float64;
  accelBiasYMps2 @29 :Float64;
  accelBiasZMps2 @30 :Float64;

  # GPS time minus IMU device time, as the arrival-time alignment has it.
  hasImuClockOffset @31 :Bool;
  imuClockOffsetS @32 :Float64;

  # The mounting as learned: roll, pitch, yaw of the IMU in the body (the
  # config's imu_to_body_rpy_deg convention), and its sigma about the body
  # axes. Yaw error here is sideslip error.
  mountingRollDeg @33 :Float64;
  mountingPitchDeg @34 :Float64;
  mountingYawDeg @35 :Float64;
  mountingSigmaRollDeg @36 :Float64;
  mountingSigmaPitchDeg @37 :Float64;
  mountingSigmaYawDeg @38 :Float64;
  # What it was learned from: seconds of straight, true running, and stops.
  mountingStraightS @39 :Float64;
  mountingLevelStops @40 :UInt32;

  # Kept between sessions. FromDatabase: this session started from what an
  # earlier one learned rather than from the config. Moved: the estimate has
  # since walked far from it -- something was knocked or re-mounted.
  calibrationStoreOk @41 :Bool;
  mountingFromDatabase @42 :Bool;
  leverArmFromDatabase @43 :Bool;
  boresightFromDatabase @44 :Bool;
  mountingMoved @45 :Bool;
  leverArmMoved @46 :Bool;
  boresightMoved @47 :Bool;
  calibrationRowsWritten @48 :UInt32;
}
