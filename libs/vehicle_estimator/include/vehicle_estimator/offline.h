#pragma once

// The whole drive at once: every keyframe the estimator made, solved
// together instead of through a window that slides and forgets.
//
// Hand OfflineSmoother the estimator's KeyframeRecords (setKeyframeSink) as
// the drive is replayed, then solve(). Every keyframe is then estimated from
// all the data before AND after it -- the forward-backward smoother, done as
// one nonlinear least-squares problem rather than an RTS pass over a
// linearised filter. What it adds shows most where the forward pass is
// weakest: the first seconds, before the biases and the lever arm have been
// learned, and across a GNSS outage, which is now bridged from both ends.

#include "vehicle_estimator/estimator.h"

#include "factor_graph/smoother.h"

#include <vector>

namespace vehicle_estimator
{

class OfflineSmoother
{
  public:
    explicit OfflineSmoother(const EstimatorConfig& config, factor_graph::LmParams lm = {});

    void add(const KeyframeRecord& record);

    struct Result
    {
        std::vector<VehicleState> states;  // one per keyframe, in time order
        factor_graph::OptimizeReport report;
        std::size_t refused = 0;  // records the batch could not take
    };

    Result solve(bool covariances = true);

    std::size_t keyframes() const { return records_.size(); }

  private:
    struct Kept
    {
        double t;
        imu_preint::KeyframeKeys keys;
        Eigen::Vector3d omega_i, f_i;
        FixQuality fix;
    };

    Estimator view_;  // for stateFrom(): the same outputs, from the batch's variables
    factor_graph::BatchSmoother batch_;
    std::vector<Kept> records_;
    std::size_t refused_ = 0;
};

}  // namespace vehicle_estimator
