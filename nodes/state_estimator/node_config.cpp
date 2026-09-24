// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_config.h"

#include <Eigen/Geometry>

#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>

#include <spdlog/spdlog.h>

#include <yaml-cpp/yaml.h>

#include "pub_sub/topic_key.h"

namespace state_estimator
{
namespace
{

constexpr double kDeg = std::numbers::pi / 180.0;

// Accumulates rather than stopping, so a config with three mistakes takes one
// run to fix rather than three.
struct Context
{
    bool ok{true};

    void fail(const std::string& message)
    {
        SPDLOG_ERROR("[config] {}", message);
        ok = false;
    }
};

void readString(const YAML::Node& parent, const char* key, std::string& out, Context& context, const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node) return;
    if (!node.IsScalar())
    {
        context.fail(where + key + " must be a string");
        return;
    }
    out = node.as<std::string>();
}

template <typename T>
void readNumber(const YAML::Node& parent, const char* key, T& out, Context& context, const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node) return;
    try
    {
        out = node.as<T>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + key + " must be a number");
        return;
    }
    if constexpr (std::is_floating_point_v<T>)
    {
        if (!std::isfinite(out)) context.fail(where + key + " must be finite");
    }
}

void readVector(const YAML::Node& parent, const char* key, Eigen::Vector3d& out, Context& context,
                const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node) return;
    if (!node.IsSequence() || node.size() != 3)
    {
        context.fail(where + key + " must be a list of three numbers");
        return;
    }
    try
    {
        for (std::size_t i = 0; i < 3; ++i) out[static_cast<Eigen::Index>(i)] = node[i].as<double>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + key + " must be a list of three numbers");
        return;
    }
    if (!out.allFinite()) context.fail(where + key + " must be finite");
}

void readBool(const YAML::Node& parent, const char* key, bool& out, Context& context, const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node) return;
    try
    {
        out = node.as<bool>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + key + " must be true or false");
    }
}

template <int N>
void readList(const YAML::Node& parent, const char* key, Eigen::Matrix<double, N, 1>& out, Context& context,
              const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node) return;
    const std::string what = where + key + " must be a list of " + std::to_string(N) + " numbers";
    if (!node.IsSequence() || node.size() != static_cast<std::size_t>(N))
    {
        context.fail(what);
        return;
    }
    try
    {
        for (Eigen::Index i = 0; i < N; ++i) out[i] = node[static_cast<std::size_t>(i)].as<double>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(what);
        return;
    }
    if (!out.allFinite()) context.fail(where + key + " must be finite");
}

const YAML::Node section(const YAML::Node& root, const char* name, Context& context)
{
    const YAML::Node node = root[name];
    if (node && !node.IsMap())
    {
        context.fail(std::string(name) + " must be a mapping");
        return YAML::Node();
    }
    return node;
}

void checkKey(const std::string& key, const char* field, Context& context)
{
    // A key outside the allowed charset fails SILENTLY -- nothing publishes,
    // nothing subscribes. Checked here so a typo is a startup error.
    if (const std::string problem = pub_sub::topicKeyProblem(key); !problem.empty())
        context.fail(std::string(field) + " ('" + key + "'): " + problem);
}

void positive(double value, const char* field, Context& context)
{
    if (!(value > 0.0)) context.fail(std::string(field) + " must be positive");
}

}  // namespace

bool parse_node_config(const std::string& yaml, NodeConfig& out)
{
    Context context;
    YAML::Node root;
    try
    {
        root = YAML::Load(yaml);
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("[config] {}", e.what());
        return false;
    }
    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] the document must be a mapping");
        return false;
    }

    if (const YAML::Node n = section(root, "inputs", context))
    {
        readString(n, "imu_prefix", out.imuPrefix, context, "inputs.");
        readString(n, "gnss_prefix", out.gnssPrefix, context, "inputs.");
    }
    if (const YAML::Node n = section(root, "outputs", context))
    {
        readString(n, "state_key", out.stateKey, context, "outputs.");
        readString(n, "status_key", out.statusKey, context, "outputs.");
        readNumber(n, "state_decimation", out.stateDecimation, context, "outputs.");
    }
    if (const YAML::Node n = section(root, "vehicle", context))
    {
        readVector(n, "imu_to_body_rpy_deg", out.imuToBodyRpyDeg, context, "vehicle.");
        readVector(n, "imu_to_body_sigma_deg", out.imuToBodySigmaDeg, context, "vehicle.");
        readVector(n, "reference_point_m", out.referencePointM, context, "vehicle.");
        readVector(n, "lever_arm_m", out.leverArmM, context, "vehicle.");
        readNumber(n, "lever_arm_sigma_m", out.leverArmSigmaM, context, "vehicle.");
        readVector(n, "antenna2_lever_arm_m", out.antenna2LeverArmM, context, "vehicle.");
        readNumber(n, "boresight_sigma_deg", out.boresightSigmaDeg, context, "vehicle.");
    }
    if (const YAML::Node n = section(root, "imu", context))
    {
        std::string frame = out.dvFrameEnd ? "end" : "start";
        readString(n, "dv_frame", frame, context, "imu.");
        if (frame == "end")
            out.dvFrameEnd = true;
        else if (frame == "start")
            out.dvFrameEnd = false;
        else
            context.fail("imu.dv_frame must be 'start' or 'end'");
        readNumber(n, "gyro_noise_density_dps", out.gyroNoiseDensityDps, context, "imu.");
        readNumber(n, "accel_noise_density_mps2", out.accelNoiseDensityMps2, context, "imu.");
        readNumber(n, "gyro_bias_walk", out.gyroBiasWalk, context, "imu.");
        readNumber(n, "accel_bias_walk", out.accelBiasWalk, context, "imu.");
        readNumber(n, "time_offset_s", out.imuTimeOffsetS, context, "imu.");
    }
    if (const YAML::Node n = section(root, "gnss", context))
    {
        double burst_ms = out.assembler.burst * 1e3;
        readNumber(n, "burst_ms", burst_ms, context, "gnss.");
        out.assembler.burst = burst_ms * 1e-3;
        readNumber(n, "velocity_sigma_horizontal", out.velocitySigmaHorizontal, context, "gnss.");
        readNumber(n, "velocity_sigma_vertical", out.velocitySigmaVertical, context, "gnss.");
        readNumber(n, "default_sigma_horizontal", out.assembler.defaultSigmaHorizontalM, context, "gnss.");
        readNumber(n, "default_sigma_vertical", out.assembler.defaultSigmaVerticalM, context, "gnss.");
    }
    if (const YAML::Node n = section(root, "smoother", context))
    {
        readNumber(n, "lag_s", out.lagS, context, "smoother.");
        readNumber(n, "max_iterations", out.maxIterations, context, "smoother.");
        readNumber(n, "time_budget_ms", out.timeBudgetMs, context, "smoother.");
    }
    if (const YAML::Node n = section(root, "output", context))
        readNumber(n, "sideslip_min_speed", out.sideslipMinSpeed, context, "output.");
    if (const YAML::Node n = section(root, "smoother", context))
        readNumber(n, "keyframe_interval_s", out.keyframeIntervalS, context, "smoother.");
    if (const YAML::Node n = section(root, "magnetometer", context))
    {
        auto& m = out.magnetometer;
        readBool(n, "enabled", m.enabled, context, "magnetometer.");
        readNumber(n, "sigma", m.sigma, context, "magnetometer.");
        readVector(n, "hard_iron", m.hardIron, context, "magnetometer.");
        readNumber(n, "hard_iron_sigma", m.hardIronSigma, context, "magnetometer.");
        readList(n, "soft_iron", m.softIron, context, "magnetometer.");
        readNumber(n, "soft_iron_sigma", m.softIronSigma, context, "magnetometer.");
        readNumber(n, "hard_iron_walk_per_sqrt_h", m.hardIronWalkPerSqrtH, context, "magnetometer.");
        readNumber(n, "soft_iron_walk_per_sqrt_h", m.softIronWalkPerSqrtH, context, "magnetometer.");
        readNumber(n, "gate_sigmas", m.gateSigmas, context, "magnetometer.");
        readNumber(n, "trust_after_s", m.trustAfterS, context, "magnetometer.");
    }
    if (const YAML::Node n = section(root, "barometer", context))
    {
        auto& b = out.barometer;
        readBool(n, "enabled", b.enabled, context, "barometer.");
        readNumber(n, "sigma_m", b.sigmaM, context, "barometer.");
        readNumber(n, "offset_m", b.offsetM, context, "barometer.");
        readNumber(n, "offset_sigma_m", b.offsetSigmaM, context, "barometer.");
        readNumber(n, "offset_walk_m_per_sqrt_h", b.offsetWalkMPerSqrtH, context, "barometer.");
        readNumber(n, "airflow", b.airflow, context, "barometer.");
        readNumber(n, "airflow_sigma", b.airflowSigma, context, "barometer.");
        readNumber(n, "airflow_walk_per_sqrt_h", b.airflowWalkPerSqrtH, context, "barometer.");
    }
    if (const YAML::Node n = section(root, "gravity", context))
    {
        readBool(n, "deflection", out.gravity.deflection, context, "gravity.");
        readString(n, "model_dir", out.gravity.modelDir, context, "gravity.");
    }
    if (const YAML::Node n = section(root, "start", context))
    {
        readBool(n, "anchored", out.start.anchored, context, "start.");
        readNumber(n, "anchor_wait_s", out.start.anchorWaitS, context, "start.");
    }
    if (const YAML::Node n = section(root, "calibration", context))
    {
        auto& c = out.calibration;
        if (const YAML::Node e = n["enabled"])
        {
            try
            {
                c.enabled = e.as<bool>();
            }
            catch (const YAML::Exception&)
            {
                context.fail("calibration.enabled must be true or false");
            }
        }
        readString(n, "database", c.database, context, "calibration.");
        readNumber(n, "min_write_interval_s", c.minWriteIntervalS, context, "calibration.");
        readNumber(n, "move_threshold_sigma", c.moveThresholdSigma, context, "calibration.");
        readNumber(n, "tighten_ratio", c.tightenRatio, context, "calibration.");
        readNumber(n, "settle_s", c.settleS, context, "calibration.");
        readNumber(n, "load_inflation", c.loadInflation, context, "calibration.");
        readNumber(n, "moved_sigma", c.movedSigma, context, "calibration.");
        readNumber(n, "segment_s", c.segmentS, context, "calibration.");
        readNumber(n, "mounting_walk_deg_per_sqrt_h", c.mountingWalkDegPerSqrtH, context, "calibration.");
        readNumber(n, "lever_arm_walk_mm_per_sqrt_h", c.leverArmWalkMmPerSqrtH, context, "calibration.");
        readNumber(n, "boresight_walk_deg_per_sqrt_h", c.boresightWalkDegPerSqrtH, context, "calibration.");
    }

    checkKey(out.imuPrefix, "inputs.imu_prefix", context);
    checkKey(out.gnssPrefix, "inputs.gnss_prefix", context);
    checkKey(out.stateKey, "outputs.state_key", context);
    checkKey(out.statusKey, "outputs.status_key", context);
    if (out.stateKey == out.statusKey) context.fail("outputs.state_key and outputs.status_key must differ");
    if (out.stateDecimation == 0) context.fail("outputs.state_decimation must be at least 1");
    positive(out.leverArmSigmaM, "vehicle.lever_arm_sigma_m", context);
    positive(out.boresightSigmaDeg, "vehicle.boresight_sigma_deg", context);
    if ((out.antenna2LeverArmM - out.leverArmM).norm() < 0.05)
        context.fail("vehicle.antenna2_lever_arm_m must be at least 5 cm from vehicle.lever_arm_m");
    positive(out.gyroNoiseDensityDps, "imu.gyro_noise_density_dps", context);
    positive(out.accelNoiseDensityMps2, "imu.accel_noise_density_mps2", context);
    positive(out.gyroBiasWalk, "imu.gyro_bias_walk", context);
    positive(out.accelBiasWalk, "imu.accel_bias_walk", context);
    if (std::fabs(out.imuTimeOffsetS) > 1.0) context.fail("imu.time_offset_s beyond a second is not a latency");
    positive(out.assembler.burst, "gnss.burst_ms", context);
    positive(out.velocitySigmaHorizontal, "gnss.velocity_sigma_horizontal", context);
    positive(out.velocitySigmaVertical, "gnss.velocity_sigma_vertical", context);
    positive(out.assembler.defaultSigmaHorizontalM, "gnss.default_sigma_horizontal", context);
    positive(out.assembler.defaultSigmaVerticalM, "gnss.default_sigma_vertical", context);
    positive(out.lagS, "smoother.lag_s", context);
    if (out.maxIterations < 1) context.fail("smoother.max_iterations must be at least 1");
    positive(out.timeBudgetMs, "smoother.time_budget_ms", context);
    for (Eigen::Index i = 0; i < 3; ++i)
        positive(out.imuToBodySigmaDeg[i], "vehicle.imu_to_body_sigma_deg", context);

    positive(out.keyframeIntervalS, "smoother.keyframe_interval_s", context);
    const auto& m = out.magnetometer;
    positive(m.sigma, "magnetometer.sigma", context);
    positive(m.hardIronSigma, "magnetometer.hard_iron_sigma", context);
    positive(m.softIronSigma, "magnetometer.soft_iron_sigma", context);
    positive(m.hardIronWalkPerSqrtH, "magnetometer.hard_iron_walk_per_sqrt_h", context);
    positive(m.softIronWalkPerSqrtH, "magnetometer.soft_iron_walk_per_sqrt_h", context);
    positive(m.gateSigmas, "magnetometer.gate_sigmas", context);
    if (!(m.trustAfterS >= 0.0)) context.fail("magnetometer.trust_after_s must not be negative");
    // (I + S) must stay invertible: a soft-iron diagonal of -1 is no sensor.
    for (Eigen::Index i = 0; i < 3; ++i)
        if (!(m.softIron[i] > -0.5)) context.fail("magnetometer.soft_iron diagonal must be above -0.5");
    const auto& b = out.barometer;
    positive(b.sigmaM, "barometer.sigma_m", context);
    positive(b.offsetSigmaM, "barometer.offset_sigma_m", context);
    positive(b.offsetWalkMPerSqrtH, "barometer.offset_walk_m_per_sqrt_h", context);
    positive(b.airflowSigma, "barometer.airflow_sigma", context);
    positive(b.airflowWalkPerSqrtH, "barometer.airflow_walk_per_sqrt_h", context);
    if (!(out.start.anchorWaitS >= 0.0)) context.fail("start.anchor_wait_s must not be negative");

    const auto& c = out.calibration;
    if (c.enabled && c.database.empty()) context.fail("calibration.database is empty; set enabled: false instead");
    if (!(c.minWriteIntervalS >= 0.0)) context.fail("calibration.min_write_interval_s must not be negative");
    positive(c.moveThresholdSigma, "calibration.move_threshold_sigma", context);
    if (!(c.tightenRatio > 0.0 && c.tightenRatio < 1.0)) context.fail("calibration.tighten_ratio must be in (0, 1)");
    if (!(c.settleS >= 0.0)) context.fail("calibration.settle_s must not be negative");
    // Below 1 a stored value would come back MORE certain than it was learned.
    if (!(c.loadInflation >= 1.0)) context.fail("calibration.load_inflation must be at least 1");
    positive(c.movedSigma, "calibration.moved_sigma", context);
    positive(c.segmentS, "calibration.segment_s", context);
    if (c.segmentS > 0.5 * out.lagS) context.fail("calibration.segment_s must be at most half of smoother.lag_s");
    positive(c.mountingWalkDegPerSqrtH, "calibration.mounting_walk_deg_per_sqrt_h", context);
    positive(c.leverArmWalkMmPerSqrtH, "calibration.lever_arm_walk_mm_per_sqrt_h", context);
    positive(c.boresightWalkDegPerSqrtH, "calibration.boresight_walk_deg_per_sqrt_h", context);
    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    std::ifstream file(path);
    if (!file)
    {
        SPDLOG_ERROR("[config] cannot read {}", path);
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    // Delegates, so the parser the tests drive is the one the node uses.
    return parse_node_config(text.str(), out);
}

vehicle_estimator::EstimatorConfig estimatorConfig(const NodeConfig& c)
{
    vehicle_estimator::EstimatorConfig e;
    // Rotation from the IMU frame to the body: ZYX from the configured
    // roll, pitch, yaw of the IMU relative to the body.
    const Eigen::Matrix3d R_b_i = (Eigen::AngleAxisd(c.imuToBodyRpyDeg.z() * kDeg, Eigen::Vector3d::UnitZ()) *
                                   Eigen::AngleAxisd(c.imuToBodyRpyDeg.y() * kDeg, Eigen::Vector3d::UnitY()) *
                                   Eigen::AngleAxisd(c.imuToBodyRpyDeg.x() * kDeg, Eigen::Vector3d::UnitX()))
                                      .toRotationMatrix();
    e.R_b_i = R_b_i;
    e.mounting_sigma = c.imuToBodySigmaDeg * kDeg;
    // Per sqrt(hour) to per sqrt(second): divide by sqrt(3600) = 60.
    e.calibration_segment = c.calibration.segmentS;
    e.mounting_walk = c.calibration.mountingWalkDegPerSqrtH * kDeg / 60.0;
    e.lever_arm_walk = c.calibration.leverArmWalkMmPerSqrtH * 1e-3 / 60.0;
    e.boresight_walk = c.calibration.boresightWalkDegPerSqrtH * kDeg / 60.0;
    e.reference_point = c.referencePointM;
    e.lever_arm = c.leverArmM;
    e.lever_arm_sigma = c.leverArmSigmaM;
    e.antenna2_lever_arm = c.antenna2LeverArmM;
    e.boresight_sigma = c.boresightSigmaDeg * kDeg;
    e.dv_frame = c.dvFrameEnd ? imu_preint::DvFrame::end : imu_preint::DvFrame::start;
    e.imu_noise.gyro_noise_density = c.gyroNoiseDensityDps * kDeg;
    e.imu_noise.accel_noise_density = c.accelNoiseDensityMps2;
    e.gyro_bias_walk = c.gyroBiasWalk;
    e.accel_bias_walk = c.accelBiasWalk;
    e.imu_time_offset = c.imuTimeOffsetS;
    e.velocity_sigma_horizontal = c.velocitySigmaHorizontal;
    e.velocity_sigma_vertical = c.velocitySigmaVertical;
    e.lag = c.lagS;
    e.lm.max_iterations = c.maxIterations;
    e.lm.time_budget = std::chrono::duration<double>(c.timeBudgetMs * 1e-3);
    e.sideslip_min_speed = c.sideslipMinSpeed;
    e.keyframe_interval = c.keyframeIntervalS;
    e.use_magnetometer = c.magnetometer.enabled;
    e.mag_sigma = c.magnetometer.sigma;
    e.mag_hard_iron = c.magnetometer.hardIron;
    e.mag_hard_iron_sigma = c.magnetometer.hardIronSigma;
    e.mag_soft_iron = c.magnetometer.softIron;
    e.mag_soft_iron_sigma = c.magnetometer.softIronSigma;
    e.mag_hard_iron_walk = c.magnetometer.hardIronWalkPerSqrtH / 60.0;
    e.mag_soft_iron_walk = c.magnetometer.softIronWalkPerSqrtH / 60.0;
    e.mag_gate_sigmas = c.magnetometer.gateSigmas;
    e.mag_trust_after = c.magnetometer.trustAfterS;
    e.use_barometer = c.barometer.enabled;
    e.baro_sigma = c.barometer.sigmaM;
    e.baro_offset = c.barometer.offsetM;
    e.baro_offset_sigma = c.barometer.offsetSigmaM;
    e.baro_offset_walk = c.barometer.offsetWalkMPerSqrtH / 60.0;
    e.baro_airflow = c.barometer.airflow;
    e.baro_airflow_sigma = c.barometer.airflowSigma;
    e.baro_airflow_walk = c.barometer.airflowWalkPerSqrtH / 60.0;
    e.anchored_start = c.start.anchored;
    e.anchor_wait = c.start.anchorWaitS;
    return e;
}

}  // namespace state_estimator
