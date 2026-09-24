#include "vehicle_estimator/calibration.h"

#include <Eigen/Cholesky>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace vehicle_estimator
{

CalibrationSet configuredCalibration(const EstimatorConfig& config)
{
    CalibrationSet c;
    c.mounting = Eigen::Quaterniond(config.R_b_i).normalized();
    c.lever_arm = config.lever_arm;
    c.boresight = Eigen::Vector2d::Zero();
    c.cov.setZero();
    // A body-axis perturbation Exp(d_b) R_b_i is R_b_i Exp(R_b_i^T d_b): the
    // IMU-frame tangent is the body one rotated back.
    const Eigen::Matrix3d R = c.mounting.toRotationMatrix();
    c.cov.block<3, 3>(0, 0) = R.transpose() * Eigen::Matrix3d(config.mounting_sigma.cwiseAbs2().asDiagonal()) * R;
    c.cov.block<3, 3>(3, 3) = std::pow(config.lever_arm_sigma, 2) * Eigen::Matrix3d::Identity();
    c.cov.block<2, 2>(6, 6) = std::pow(config.boresight_sigma, 2) * Eigen::Matrix2d::Identity();
    c.mag_hard_iron = config.mag_hard_iron;
    c.mag_soft_iron = config.mag_soft_iron;
    c.cov.block<3, 3>(8, 8) = std::pow(config.mag_hard_iron_sigma, 2) * Eigen::Matrix3d::Identity();
    c.cov.block<6, 6>(11, 11) = std::pow(config.mag_soft_iron_sigma, 2) * Eigen::Matrix<double, 6, 6>::Identity();
    c.baro_offset = config.baro_offset;
    c.baro_airflow = config.baro_airflow;
    c.cov(17, 17) = std::pow(config.baro_offset_sigma, 2);
    c.cov(18, 18) = std::pow(config.baro_airflow_sigma, 2);
    return c;
}

Eigen::Matrix3d CalibrationSet::softIronMatrix() const
{
    const Vector6d& s = mag_soft_iron;
    Eigen::Matrix3d m;
    m << 1.0 + s[0], s[3], s[4],  //
        s[3], 1.0 + s[1], s[5],    //
        s[4], s[5], 1.0 + s[2];
    return m;
}

Eigen::Matrix<double, kCalibrationDim, 1> calibrationWalkDensities(const EstimatorConfig& c)
{
    Eigen::Matrix<double, kCalibrationDim, 1> q;
    q << Eigen::Vector3d::Constant(c.mounting_walk), Eigen::Vector3d::Constant(c.lever_arm_walk),
        Eigen::Vector2d::Constant(c.boresight_walk), Eigen::Vector3d::Constant(c.mag_hard_iron_walk),
        Vector6d::Constant(c.mag_soft_iron_walk), c.baro_offset_walk, c.baro_airflow_walk;
    return q;
}

CalibrationKeys calibrationKeys(std::uint64_t segment)
{
    return {factor_graph::symbol('m', segment), factor_graph::symbol('l', segment), factor_graph::symbol('s', segment),
            factor_graph::symbol('k', segment), factor_graph::symbol('o', segment)};
}

bool isCalibrationKey(factor_graph::Key key)
{
    const char kind = factor_graph::symbolKind(key);
    return kind == 'm' || kind == 'l' || kind == 's' || kind == 'k' || kind == 'o';
}

// ---- groups ----------------------------------------------------------------

namespace
{

constexpr double kDeg = std::numbers::pi / 180.0;

Eigen::Quaterniond quaternionOf(const Eigen::VectorXd& mean)
{
    return Eigen::Quaterniond(mean[0], mean[1], mean[2], mean[3]);
}

}  // namespace

std::string_view groupName(CalibrationGroup g)
{
    switch (g)
    {
        case CalibrationGroup::mounting:
            return "mounting";
        case CalibrationGroup::lever_arm:
            return "lever_arm";
        case CalibrationGroup::boresight:
            return "boresight";
        case CalibrationGroup::magnetometer:
            return "magnetometer";
        case CalibrationGroup::baro_airflow:
            return "baro_airflow";
    }
    return "unknown";
}

std::optional<CalibrationGroup> groupFromName(std::string_view name)
{
    for (CalibrationGroup g : kCalibrationGroups)
        if (groupName(g) == name) return g;
    return std::nullopt;
}

int modelVersion(CalibrationGroup g)
{
    switch (g)
    {
        case CalibrationGroup::mounting:
            return 1;
        case CalibrationGroup::lever_arm:
            return 1;
        case CalibrationGroup::boresight:
            return 1;
        case CalibrationGroup::magnetometer:
            return 1;
        case CalibrationGroup::baro_airflow:
            return 1;
    }
    return 0;
}

Eigen::Index tangentOffset(CalibrationGroup g)
{
    switch (g)
    {
        case CalibrationGroup::mounting:
            return 0;
        case CalibrationGroup::lever_arm:
            return 3;
        case CalibrationGroup::boresight:
            return 6;
        case CalibrationGroup::magnetometer:
            return 8;
        case CalibrationGroup::baro_airflow:
            return 18;  // 17 is the offset, which is not kept
    }
    return 0;
}

Eigen::Index tangentDim(CalibrationGroup g)
{
    switch (g)
    {
        case CalibrationGroup::mounting:
        case CalibrationGroup::lever_arm:
            return 3;
        case CalibrationGroup::boresight:
            return 2;
        case CalibrationGroup::magnetometer:
            return 9;
        case CalibrationGroup::baro_airflow:
            return 1;
    }
    return 0;
}

Eigen::Index meanDim(CalibrationGroup g)
{
    switch (g)
    {
        case CalibrationGroup::mounting:
            return 4;
        case CalibrationGroup::lever_arm:
            return 3;
        case CalibrationGroup::boresight:
            return 2;
        case CalibrationGroup::magnetometer:
            return 9;
        case CalibrationGroup::baro_airflow:
            return 1;
    }
    return 0;
}

GroupEstimate extract(const CalibrationSet& set, CalibrationGroup g)
{
    GroupEstimate e;
    e.group = g;
    const Eigen::Index o = tangentOffset(g), n = tangentDim(g);
    e.cov = set.cov.block(o, o, n, n);
    switch (g)
    {
        case CalibrationGroup::mounting:
        {
            const Eigen::Quaterniond q = set.mounting.normalized();
            e.mean = Eigen::Vector4d(q.w(), q.x(), q.y(), q.z());
            break;
        }
        case CalibrationGroup::lever_arm:
            e.mean = set.lever_arm;
            break;
        case CalibrationGroup::boresight:
            e.mean = set.boresight;
            break;
        case CalibrationGroup::magnetometer:
            e.mean = set.magnetometer();
            break;
        case CalibrationGroup::baro_airflow:
            e.mean = Eigen::VectorXd::Constant(1, set.baro_airflow);
            break;
    }
    return e;
}

void apply(CalibrationSet& set, const GroupEstimate& e)
{
    const Eigen::Index o = tangentOffset(e.group), n = tangentDim(e.group);
    switch (e.group)
    {
        case CalibrationGroup::mounting:
            set.mounting = quaternionOf(e.mean).normalized();
            break;
        case CalibrationGroup::lever_arm:
            set.lever_arm = e.mean;
            break;
        case CalibrationGroup::boresight:
            set.boresight = e.mean;
            break;
        case CalibrationGroup::magnetometer:
            set.mag_hard_iron = e.mean.head<3>();
            set.mag_soft_iron = e.mean.tail<6>();
            break;
        case CalibrationGroup::baro_airflow:
            set.baro_airflow = e.mean[0];
            break;
    }
    set.cov.block(o, 0, n, kCalibrationDim).setZero();
    set.cov.block(0, o, kCalibrationDim, n).setZero();
    set.cov.block(o, o, n, n) = e.cov;
}

std::optional<std::string> problem(const GroupEstimate& e)
{
    const Eigen::Index n = tangentDim(e.group);
    if (e.mean.size() != meanDim(e.group))
        return std::format("{} mean has {} values, not {}", groupName(e.group), e.mean.size(), meanDim(e.group));
    if (e.cov.rows() != n || e.cov.cols() != n)
        return std::format("{} covariance is {}x{}, not {}x{}", groupName(e.group), e.cov.rows(), e.cov.cols(), n, n);
    if (!e.mean.allFinite() || !e.cov.allFinite()) return std::format("{} is not finite", groupName(e.group));
    if (e.group == CalibrationGroup::mounting && std::fabs(e.mean.norm() - 1.0) > 1e-6)
        return std::format("mounting quaternion has norm {}", e.mean.norm());
    if ((e.cov - e.cov.transpose()).cwiseAbs().maxCoeff() > 1e-9 * (1.0 + e.cov.cwiseAbs().maxCoeff()))
        return std::format("{} covariance is not symmetric", groupName(e.group));
    if (Eigen::LLT<Eigen::MatrixXd>(e.cov).info() != Eigen::Success)
        return std::format("{} covariance is not positive definite", groupName(e.group));
    return std::nullopt;
}

Eigen::VectorXd tangentDifference(const GroupEstimate& a, const GroupEstimate& b)
{
    switch (a.group)
    {
        case CalibrationGroup::mounting:
        {
            // Right perturbation, as the smoother's tangent: b = a Exp(d).
            const Eigen::AngleAxisd d(quaternionOf(a.mean).normalized().conjugate() * quaternionOf(b.mean).normalized());
            return Eigen::Vector3d(d.angle() * d.axis());
        }
        case CalibrationGroup::lever_arm:
        case CalibrationGroup::boresight:
        case CalibrationGroup::magnetometer:
        case CalibrationGroup::baro_airflow:
            return b.mean - a.mean;
    }
    return {};
}

std::string summary(const GroupEstimate& e)
{
    const Eigen::VectorXd sigma = e.cov.diagonal().cwiseMax(0.0).cwiseSqrt();
    switch (e.group)
    {
        case CalibrationGroup::mounting:
        {
            const Eigen::Quaterniond q = quaternionOf(e.mean).normalized();
            const Eigen::Vector3d rpy = mountingRpy(q) / kDeg;
            const Eigen::Vector3d sb = mountingSigmaBody(q, e.cov) / kDeg;
            return std::format("roll {:.3f} pitch {:.3f} yaw {:.3f} deg, sigma {:.3f} {:.3f} {:.3f} deg", rpy.x(),
                               rpy.y(), rpy.z(), sb.x(), sb.y(), sb.z());
        }
        case CalibrationGroup::lever_arm:
            return std::format("x {:.4f} y {:.4f} z {:.4f} m, sigma {:.4f} {:.4f} {:.4f} m", e.mean[0], e.mean[1],
                               e.mean[2], sigma[0], sigma[1], sigma[2]);
        case CalibrationGroup::boresight:
            return std::format("{:.3f} {:.3f} deg, sigma {:.3f} {:.3f} deg", e.mean[0] / kDeg, e.mean[1] / kDeg,
                               sigma[0] / kDeg, sigma[1] / kDeg);
        case CalibrationGroup::magnetometer:
            return std::format("hard iron {:.4f} {:.4f} {:.4f} (sigma {:.4f} {:.4f} {:.4f}), soft iron diag {:.4f} "
                               "{:.4f} {:.4f} off {:.4f} {:.4f} {:.4f}",
                               e.mean[0], e.mean[1], e.mean[2], sigma[0], sigma[1], sigma[2], e.mean[3], e.mean[4],
                               e.mean[5], e.mean[6], e.mean[7], e.mean[8]);
        case CalibrationGroup::baro_airflow:
            return std::format("{:.3f} of dynamic pressure, sigma {:.3f}", e.mean[0], sigma[0]);
    }
    return {};
}

// ---- the policy ----------------------------------------------------------------

namespace
{

class Fnv1a
{
  public:
    void bytes(const void* data, std::size_t n)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i)
        {
            hash_ ^= p[i];
            hash_ *= 1099511628211ULL;
        }
    }
    void text(std::string_view s)
    {
        bytes(s.data(), s.size());
        bytes("", 1);  // a separator, so "ab"+"c" is not "a"+"bc"
    }
    void integer(std::int64_t v)
    {
        // Little-endian whatever the host, so a hash is the same on the car
        // and on a workstation.
        for (int i = 0; i < 8; ++i)
        {
            const auto b = static_cast<unsigned char>((static_cast<std::uint64_t>(v) >> (8 * i)) & 0xFF);
            bytes(&b, 1);
        }
    }
    void number(double v)
    {
        if (v == 0.0) v = 0.0;  // -0.0 is the same measurement
        integer(std::bit_cast<std::int64_t>(v));
    }
    void numbers(const Eigen::MatrixXd& m)
    {
        for (Eigen::Index r = 0; r < m.rows(); ++r)
            for (Eigen::Index c = 0; c < m.cols(); ++c) number(m(r, c));
    }
    std::uint64_t value() const { return hash_; }

  private:
    std::uint64_t hash_ = 14695981039346656037ULL;
};

}  // namespace

std::uint64_t priorHash(CalibrationGroup g, const EstimatorConfig& config)
{
    Fnv1a h;
    h.text("redline calibration prior");
    h.text(groupName(g));
    h.integer(modelVersion(g));
    switch (g)
    {
        case CalibrationGroup::mounting:
            h.numbers(config.R_b_i);
            break;
        case CalibrationGroup::lever_arm:
            h.numbers(config.lever_arm);
            break;
        case CalibrationGroup::boresight:
            h.numbers(config.lever_arm);
            h.numbers(config.antenna2_lever_arm);
            break;
        case CalibrationGroup::magnetometer:
            h.numbers(config.mag_hard_iron);
            h.numbers(config.mag_soft_iron);
            break;
        case CalibrationGroup::baro_airflow:
            h.number(config.baro_airflow);
            break;
    }
    return h.value();
}

std::string hashHex(std::uint64_t hash)
{
    return std::format("{:016x}", hash);
}

std::string_view reasonName(WriteReason r)
{
    switch (r)
    {
        case WriteReason::first_converged:
            return "first_converged";
        case WriteReason::moved:
            return "moved";
        case WriteReason::tightened:
            return "tightened";
        case WriteReason::shutdown_moved:
            return "shutdown_moved";
        case WriteReason::shutdown_tightened:
            return "shutdown_tightened";
    }
    return "unknown";
}

namespace
{

double mahalanobis(const Eigen::VectorXd& d, const Eigen::MatrixXd& cov)
{
    const Eigen::LLT<Eigen::MatrixXd> llt(cov);
    if (llt.info() != Eigen::Success) return std::numeric_limits<double>::infinity();
    return std::sqrt(std::max(0.0, d.dot(llt.solve(d))));
}

}  // namespace

std::optional<WriteReason> decideWrite(const WritePolicy& policy, const GroupEstimate& current,
                                       const std::optional<LastWritten>& last, const WriteContext& c)
{
    if (!c.valid || c.settled < policy.settle || problem(current)) return std::nullopt;
    // A group nothing has taught is its prior restated: not worth a row. The
    // mounting learns only on straights and stops, the magnetometer only with
    // a heading to learn against, the airflow only at speed.
    const bool needs_evidence = current.group == CalibrationGroup::mounting ||
                                current.group == CalibrationGroup::magnetometer ||
                                current.group == CalibrationGroup::baro_airflow;
    if (needs_evidence && !(c.evidence > 0.0)) return std::nullopt;
    if (!last) return WriteReason::first_converged;
    if (!c.shutdown && last->at && c.now - *last->at < policy.min_interval) return std::nullopt;

    const auto& was = last->estimate;
    const bool moved = mahalanobis(tangentDifference(was, current), was.cov) > policy.move_sigma;
    // Tightened on any axis: the ratio of the old sigma to the new.
    const Eigen::VectorXd ratio = current.cov.diagonal().cwiseMax(1e-300).cwiseInverse().cwiseProduct(was.cov.diagonal());
    const bool tightened = ratio.maxCoeff() >= 1.0 / (policy.tighten_ratio * policy.tighten_ratio);
    if (moved) return c.shutdown ? WriteReason::shutdown_moved : WriteReason::moved;
    if (tightened) return c.shutdown ? WriteReason::shutdown_tightened : WriteReason::tightened;
    return std::nullopt;
}

GroupEstimate loadPrior(const GroupEstimate& stored, const EstimatorConfig& config, double inflation)
{
    GroupEstimate out = stored;
    const GroupEstimate configured = extract(configuredCalibration(config), stored.group);
    // No tighter than a few hundredths of a degree or a couple of millimetres,
    // however long the drive that learned it: the car is not the same car
    // to that precision from one day to the next.
    double floor = 0.0;
    switch (stored.group)
    {
        case CalibrationGroup::mounting:
        case CalibrationGroup::boresight:
            floor = 0.02 * kDeg;
            break;
        case CalibrationGroup::lever_arm:
            floor = 0.002;
            break;
        case CalibrationGroup::magnetometer:
            floor = 0.002;  // a.u., and dimensionless for the soft iron
            break;
        case CalibrationGroup::baro_airflow:
            floor = 0.01;
            break;
    }
    const Eigen::Index n = tangentDim(stored.group);
    out.cov = inflation * stored.cov + floor * floor * Eigen::MatrixXd::Identity(n, n);
    // ...and never looser than the tape measure: past that, the config is the
    // better prior and the stored value has nothing to add.
    const double worst = out.cov.diagonal().cwiseQuotient(configured.cov.diagonal()).maxCoeff();
    if (worst > 1.0) out.cov /= worst;
    return out;
}

double distanceFrom(const GroupEstimate& loaded, const GroupEstimate& current)
{
    return mahalanobis(tangentDifference(loaded, current), loaded.cov + current.cov);
}

Eigen::Vector3d mountingRpy(const Eigen::Quaterniond& q)
{
    // R = Rz(yaw) Ry(pitch) Rx(roll): the same ZYX decomposition as attitude.
    const Eigen::Matrix3d R = q.normalized().toRotationMatrix();
    const double pitch = std::asin(std::clamp(-R(2, 0), -1.0, 1.0));
    return {std::atan2(R(2, 1), R(2, 2)), pitch, std::atan2(R(1, 0), R(0, 0))};
}

Eigen::Vector3d mountingSigmaBody(const Eigen::Quaterniond& q, const Eigen::Matrix3d& cov_i)
{
    const Eigen::Matrix3d R = q.normalized().toRotationMatrix();
    return (R * cov_i * R.transpose()).diagonal().cwiseMax(0.0).cwiseSqrt();
}

}  // namespace vehicle_estimator
