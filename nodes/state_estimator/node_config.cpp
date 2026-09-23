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
    return e;
}

}  // namespace state_estimator
