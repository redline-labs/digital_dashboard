// SPDX-License-Identifier: GPL-3.0-or-later
//
// The estimator's learned installation, kept between sessions.
//
// At start: for each group (mounting, lever arm, boresight) the newest stored
// row learned against the priors in today's config -- matched by a hash of
// those priors, so a re-measured lever arm stops finding the old one -- is
// loaded, its covariance widened, and handed to the estimator as the prior
// for its next start.
//
// While running: once a second, vehicle_estimator's write policy decides per
// group whether the estimate has moved or tightened enough since the last row
// to be worth another, at most once per interval; at shutdown, once more
// without the interval. And if the estimate walks far from what was loaded,
// the group is flagged as moved since the last session -- an antenna knocked
// or an IMU re-mounted -- for the node's health.
//
// The session's clock is the estimator's GPS time, so a replay decides
// exactly as the drive did. Rows are stamped with the wall clock only for a
// person reading the history.

#ifndef STATE_ESTIMATOR_CALIBRATION_KEEPER_H
#define STATE_ESTIMATOR_CALIBRATION_KEEPER_H

#include "node_config.h"

#include "calibration_store/store.h"
#include "vehicle_estimator/calibration.h"
#include "vehicle_estimator/estimator.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace state_estimator
{

struct CalibrationReport
{
    bool store_open = false;
    std::string store_error;  // why it is not, when it is not
    // Indexed as vehicle_estimator::kCalibrationGroups.
    std::array<bool, 3> from_database{};
    std::array<bool, 3> moved{};        // latched once past moved_sigma
    std::array<double, 3> moved_by{};   // sigmas from what was loaded, now
    std::uint32_t rows_written = 0;
    std::uint32_t rows_skipped = 0;     // malformed rows passed over at load
};

class CalibrationKeeper
{
  public:
    using WallClock = std::function<std::int64_t()>;  // ns since the epoch
    CalibrationKeeper(const NodeConfig& config, std::string session, WallClock wall = {});

    // The configured path with ${REDLINE_DATA_DIR} and ~ expanded.
    static std::filesystem::path databasePath(const NodeConfig& config);

    // False when the store cannot be opened; the reason is in report(). The
    // node then runs on its config, which is what it did before there was a
    // store, and says so in its health.
    bool open(const std::filesystem::path& path);

    // Loads what earlier sessions learned into the estimator's next start.
    void seed(vehicle_estimator::Estimator& estimator);

    // The write policy, every group; `shutdown` is the last call.
    void tick(const vehicle_estimator::Estimator& estimator, bool shutdown = false);

    const CalibrationReport& report() const { return report_; }

  private:
    vehicle_estimator::EstimatorConfig config_;
    vehicle_estimator::WritePolicy policy_;
    double inflation_;
    double moved_sigma_;
    std::string session_;
    WallClock wall_;

    std::optional<calibration_store::Store> store_;
    std::array<std::optional<vehicle_estimator::LastWritten>, 3> last_;
    std::array<std::optional<vehicle_estimator::GroupEstimate>, 3> loaded_;
    std::optional<double> started_, first_;
    std::uint64_t seen_resets_ = 0;
    CalibrationReport report_;
};

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_CALIBRATION_KEEPER_H
