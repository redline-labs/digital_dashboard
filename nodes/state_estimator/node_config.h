// SPDX-License-Identifier: GPL-3.0-or-later
//
// configs/state_estimator/state_estimator.yaml, as a struct.
//
// Everything that describes the car -- where the IMU sits, which way it
// faces, where the antennas are, what point to report -- lives here, because
// every one of those numbers turns into a wrong sideslip angle if it is
// wrong, and a wrong sideslip angle looks exactly like a right one.

#ifndef STATE_ESTIMATOR_NODE_CONFIG_H
#define STATE_ESTIMATOR_NODE_CONFIG_H

#include "gnss_assembler.h"

#include "vehicle_estimator/config.h"

#include <Eigen/Core>

#include <string>

namespace state_estimator
{

struct NodeConfig
{
    // Inputs: each bridge's prefix, subscribed with a wildcard and dispatched
    // on the schema every sample carries.
    std::string imuPrefix{"nodes/mti610/mtdata2"};
    std::string gnssPrefix{"nodes/bd992/gsof"};

    // Outputs.
    std::string stateKey{"nodes/state_estimator/state"};
    std::string statusKey{"nodes/state_estimator/status"};
    // Every IMU sample produces a state; publish one in `stateDecimation`.
    unsigned stateDecimation{1};

    // The vehicle, all in metres and degrees, in the IMU's own frame.
    Eigen::Vector3d imuToBodyRpyDeg{180.0, 0.0, 0.0};  // MTi mounted x forward, z up
    Eigen::Vector3d imuToBodySigmaDeg{2.0, 2.0, 2.0};  // about the body x, y, z
    Eigen::Vector3d referencePointM{Eigen::Vector3d::Zero()};
    Eigen::Vector3d leverArmM{Eigen::Vector3d::Zero()};
    double leverArmSigmaM{0.02};
    Eigen::Vector3d antenna2LeverArmM{-1.0, 0.0, 0.0};
    double boresightSigmaDeg{1.0};

    // The IMU.
    bool dvFrameEnd{true};
    double gyroNoiseDensityDps{0.007};     // deg/s/sqrt(Hz)
    double accelNoiseDensityMps2{5.9e-4};  // m/s^2/sqrt(Hz)
    double gyroBiasWalk{2e-5};             // rad/s/sqrt(s)
    double accelBiasWalk{2e-4};            // m/s^2/sqrt(s)
    double imuTimeOffsetS{0.0};

    // The GNSS.
    AssemblerOptions assembler;
    double velocitySigmaHorizontal{0.03};
    double velocitySigmaVertical{0.06};

    // The smoother.
    double lagS{3.0};
    int maxIterations{8};
    double timeBudgetMs{40.0};

    double sideslipMinSpeed{2.0};
    double keyframeIntervalS{0.1};  // keyframes on the IMU's clock when GNSS is not making them

    // Gravity leaned by NGS's DEFLEC2022 deflection of the vertical, loaded at
    // start from the verbatim model files. An empty modelDir finds them where
    // they install (core::paths::resource("models/deflec2022"), beside bin/)
    // or in the checkout; a set one expands ${REDLINE_DATA_DIR} and ~.
    struct Gravity
    {
        bool deflection{true};
        std::string modelDir;
    };
    Gravity gravity;

    // The MTi's magnetometer, in its own arbitrary units (about 1 at the field
    // it was calibrated in). Hard and soft iron are priors, learned against the
    // dual-antenna heading and kept between sessions.
    struct Magnetometer
    {
        bool enabled{true};
        double sigma{0.03};  // per axis: about 3 deg of heading
        Eigen::Vector3d hardIron{Eigen::Vector3d::Zero()};
        double hardIronSigma{0.3};
        Eigen::Matrix<double, 6, 1> softIron{Eigen::Matrix<double, 6, 1>::Zero()};  // xx yy zz xy xz yz
        double softIronSigma{0.1};
        double hardIronWalkPerSqrtH{0.01};
        double softIronWalkPerSqrtH{0.005};
        double gateSigmas{5.0};
        double trustAfterS{30.0};
    };
    Magnetometer magnetometer;

    // The MTi's barometer. The offset -- weather and geoid -- is learned each
    // session from this prior and never kept; the airflow is kept.
    struct Barometer
    {
        bool enabled{true};
        double sigmaM{0.5};
        double offsetM{0.0};
        double offsetSigmaM{300.0};
        double offsetWalkMPerSqrtH{5.0};
        double airflow{0.0};
        double airflowSigma{0.5};
        double airflowWalkPerSqrtH{0.01};
    };
    Barometer barometer;

    // With no GNSS at all: attitude only, from gravity and (if one was
    // learned) the magnetometer, until the first fix.
    struct Start
    {
        bool anchored{true};
        double anchorWaitS{1.0};
    };
    Start start;

    // What was learned about the installation, kept between sessions.
    struct Calibration
    {
        bool enabled{true};
        // ${REDLINE_DATA_DIR} expands (core::paths::expand) when the node opens it.
        std::string database{"${REDLINE_DATA_DIR}/state_estimator/calibration.sqlite"};
        double minWriteIntervalS{900.0};
        double moveThresholdSigma{1.0};
        double tightenRatio{0.5};
        double settleS{120.0};
        double loadInflation{4.0};
        double movedSigma{4.0};
        // The walk, in units a person can judge.
        double segmentS{1.0};
        double mountingWalkDegPerSqrtH{0.05};
        double leverArmWalkMmPerSqrtH{5.0};
        double boresightWalkDegPerSqrtH{0.05};
    };
    Calibration calibration;
};

bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

// What the library takes.
vehicle_estimator::EstimatorConfig estimatorConfig(const NodeConfig& config);

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_NODE_CONFIG_H
