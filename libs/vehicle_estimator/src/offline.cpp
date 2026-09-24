#include "vehicle_estimator/offline.h"

#include "csym/geo/rot3.h"

#include <algorithm>

namespace vehicle_estimator
{

namespace
{

Eigen::Vector3d toE(const csym::Vector3<double>& v)
{
    return {v[0], v[1], v[2]};
}

Eigen::Quaterniond toE(const csym::Rot3<double>& r)
{
    return Eigen::Quaterniond(r.w, r.x, r.y, r.z).normalized();
}

}  // namespace

OfflineSmoother::OfflineSmoother(const EstimatorConfig& config, factor_graph::LmParams lm)
    : view_(config), batch_([&] {
          // A batch has time the car does not: iterate to convergence.
          lm.max_iterations = std::max(lm.max_iterations, 50);
          return lm;
      }()),
      config_(config)
{
}

void OfflineSmoother::add(const KeyframeRecord& r)
{
    factor_graph::Values values;
    factor_graph::FactorList factors;
    for (factor_graph::Key k : r.values.keys()) values.insert(k, r.values.variable(k).clone());

    // A restart re-declares the installation with a prior that restates what
    // the forward pass had learned; the batch already has all of that. It
    // drops the prior and walks on from the last segment it has instead.
    const bool link = r.start && !segments_.empty();
    for (const auto& f : r.factors)
    {
        const auto keys = f->keys();
        const bool only_calibration = std::all_of(keys.begin(), keys.end(), isCalibrationKey);
        if (link && only_calibration) continue;
        factors.push_back(f);
    }
    if (link)
    {
        const Segment& last = segments_.back();
        factors.push_back(factors::calibrationWalk(calibrationKeys(last.index), calibrationKeys(r.segment),
                                                   std::max(r.segment_start - last.start, 1e-3), config_.mounting_walk,
                                                   config_.lever_arm_walk, config_.boresight_walk));
    }
    if (batch_.add(factors, values).ok)
    {
        records_.push_back({r.t, r.keys, r.omega_i, r.f_i, r.fix, r.segment});
        if (segments_.empty() || segments_.back().index != r.segment) segments_.push_back({r.segment, r.segment_start});
    }
    else
    {
        ++refused_;
    }
}

OfflineSmoother::Result OfflineSmoother::solve(bool covariances)
{
    Result out;
    out.refused = refused_;
    out.report = batch_.optimize();
    const auto& est = batch_.estimate();

    // One factorisation for every group: each keyframe's [R, p, v, mounting],
    // then each segment's installation.
    std::vector<std::optional<Eigen::MatrixXd>> covs(records_.size() + segments_.size());
    if (covariances)
    {
        std::vector<std::vector<factor_graph::Key>> groups;
        groups.reserve(covs.size());
        for (const auto& r : records_) groups.push_back({r.keys.R, r.keys.p, r.keys.v, calibrationKeys(r.segment).mounting});
        for (const auto& s : segments_)
        {
            const auto k = calibrationKeys(s.index);
            groups.push_back({k.mounting, k.lever_arm, k.boresight});
        }
        covs = factor_graph::jointCovariances(batch_.factors(), est, groups);
    }

    out.states.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i)
    {
        const auto& r = records_[i];
        std::optional<Eigen::MatrixXd> cov_Rpv;
        std::optional<Eigen::Matrix3d> mounting_cov;
        if (covs[i])
        {
            cov_Rpv = covs[i]->topLeftCorner(9, 9);
            mounting_cov = covs[i]->block<3, 3>(9, 9);
        }
        VehicleState s = view_.stateFrom(
            r.t, toE(est.at<csym::Rot3<double>>(r.keys.R)), toE(est.at<csym::Vector3<double>>(r.keys.p)),
            toE(est.at<csym::Vector3<double>>(r.keys.v)), toE(est.at<csym::Vector3<double>>(r.keys.bg)),
            toE(est.at<csym::Vector3<double>>(r.keys.ba)), r.omega_i, r.f_i,
            toE(est.at<csym::Rot3<double>>(calibrationKeys(r.segment).mounting)), cov_Rpv, mounting_cov);
        s.fix = r.fix;
        out.states.push_back(s);
    }

    out.calibration.reserve(segments_.size());
    for (std::size_t j = 0; j < segments_.size(); ++j)
    {
        const auto k = calibrationKeys(segments_[j].index);
        Result::Calibration c;
        c.t = segments_[j].start;
        c.set.mounting = toE(est.at<csym::Rot3<double>>(k.mounting));
        c.set.lever_arm = toE(est.at<csym::Vector3<double>>(k.lever_arm));
        const auto bs = est.at<csym::Vector2<double>>(k.boresight);
        c.set.boresight = Eigen::Vector2d(bs[0], bs[1]);
        if (const auto& cov = covs[records_.size() + j]) c.set.cov = *cov;
        out.calibration.push_back(c);
    }
    return out;
}

}  // namespace vehicle_estimator
