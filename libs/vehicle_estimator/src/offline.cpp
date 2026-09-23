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

}  // namespace

OfflineSmoother::OfflineSmoother(const EstimatorConfig& config, factor_graph::LmParams lm)
    : view_(config), batch_([&] {
          // A batch has time the car does not: iterate to convergence.
          lm.max_iterations = std::max(lm.max_iterations, 50);
          return lm;
      }())
{
}

void OfflineSmoother::add(const KeyframeRecord& r)
{
    factor_graph::Values values;
    factor_graph::FactorList factors;
    const auto isStatic = [](factor_graph::Key k) {
        return k == Estimator::leverArmKey() || k == Estimator::boresightKey();
    };
    const bool statics_known = batch_.estimate().contains(Estimator::leverArmKey());
    for (factor_graph::Key k : r.values.keys())
    {
        // A restart re-declares the static variables; the batch keeps the
        // ones it has.
        if (statics_known && isStatic(k)) continue;
        values.insert(k, r.values.variable(k).clone());
    }
    for (const auto& f : r.factors)
    {
        const auto keys = f->keys();
        const bool only_static = std::all_of(keys.begin(), keys.end(), isStatic);
        // ...and drops the priors that restated them (see KeyframeRecord::start).
        if (r.start && statics_known && only_static) continue;
        factors.push_back(f);
    }
    if (batch_.add(factors, values).ok)
        records_.push_back({r.t, r.keys, r.omega_i, r.f_i, r.fix});
    else
        ++refused_;
}

OfflineSmoother::Result OfflineSmoother::solve(bool covariances)
{
    Result out;
    out.refused = refused_;
    out.report = batch_.optimize();
    const auto& est = batch_.estimate();

    std::vector<std::optional<Eigen::MatrixXd>> covs(records_.size());
    if (covariances)
    {
        std::vector<std::vector<factor_graph::Key>> groups;
        groups.reserve(records_.size());
        for (const auto& r : records_) groups.push_back({r.keys.R, r.keys.p, r.keys.v});
        covs = factor_graph::jointCovariances(batch_.factors(), est, groups);
    }

    out.states.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i)
    {
        const auto& r = records_[i];
        const auto R = est.at<csym::Rot3<double>>(r.keys.R);
        VehicleState s = view_.stateFrom(r.t, Eigen::Quaterniond(R.w, R.x, R.y, R.z).normalized(),
                                         toE(est.at<csym::Vector3<double>>(r.keys.p)),
                                         toE(est.at<csym::Vector3<double>>(r.keys.v)),
                                         toE(est.at<csym::Vector3<double>>(r.keys.bg)),
                                         toE(est.at<csym::Vector3<double>>(r.keys.ba)), r.omega_i, r.f_i, covs[i]);
        s.fix = r.fix;
        out.states.push_back(s);
    }
    return out;
}

}  // namespace vehicle_estimator
