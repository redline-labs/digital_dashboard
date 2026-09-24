// SPDX-License-Identifier: GPL-3.0-or-later

#include "calibration_keeper.h"

#include "core/core.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <span>
#include <utility>

namespace state_estimator
{

namespace
{

namespace ve = vehicle_estimator;

std::size_t indexOf(ve::CalibrationGroup g)
{
    switch (g)
    {
        case ve::CalibrationGroup::mounting:
            return 0;
        case ve::CalibrationGroup::lever_arm:
            return 1;
        case ve::CalibrationGroup::boresight:
            return 2;
        case ve::CalibrationGroup::magnetometer:
            return 3;
        case ve::CalibrationGroup::baro_airflow:
            return 4;
    }
    return 0;
}

// What each group was learned from, in the units its row records: the
// mounting on straights and stops, the magnetometer against a dual-antenna
// heading, the airflow at speed; the rest from any valid driving.
double evidenceFor(ve::CalibrationGroup g, const ve::EstimatorStatus& status, double driving)
{
    switch (g)
    {
        case ve::CalibrationGroup::mounting:
            return status.mount_straight_s + static_cast<double>(status.mount_level_stops);
        case ve::CalibrationGroup::magnetometer:
            return status.mag_learning_s;
        case ve::CalibrationGroup::baro_airflow:
            return status.baro_moving_s;
        case ve::CalibrationGroup::lever_arm:
        case ve::CalibrationGroup::boresight:
            return driving;
    }
    return driving;
}

std::vector<double> toVector(const Eigen::VectorXd& v)
{
    return {v.data(), v.data() + v.size()};
}

std::vector<double> toRowMajor(const Eigen::MatrixXd& m)
{
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(m.size()));
    for (Eigen::Index r = 0; r < m.rows(); ++r)
        for (Eigen::Index c = 0; c < m.cols(); ++c) out.push_back(m(r, c));
    return out;
}

// A stored row as an estimate, or what is wrong with it.
std::optional<ve::GroupEstimate> fromRow(ve::CalibrationGroup g, const calibration_store::Row& row, std::string* why)
{
    ve::GroupEstimate e;
    e.group = g;
    const auto n = static_cast<Eigen::Index>(std::llround(std::sqrt(static_cast<double>(row.cov.size()))));
    if (n * n != static_cast<Eigen::Index>(row.cov.size()))
    {
        if (why) *why = "covariance is not square";
        return std::nullopt;
    }
    e.mean = Eigen::Map<const Eigen::VectorXd>(row.mean.data(), static_cast<Eigen::Index>(row.mean.size()));
    e.cov = Eigen::Map<const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(row.cov.data(), n, n);
    if (const auto p = ve::problem(e))
    {
        if (why) *why = *p;
        return std::nullopt;
    }
    return e;
}

}  // namespace

CalibrationKeeper::CalibrationKeeper(const NodeConfig& config, std::string session, WallClock wall)
    : config_(estimatorConfig(config)),
      inflation_(config.calibration.loadInflation),
      moved_sigma_(config.calibration.movedSigma),
      session_(std::move(session)),
      wall_(wall ? std::move(wall) : WallClock([] {
          return std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
              .count();
      }))
{
    policy_.min_interval = config.calibration.minWriteIntervalS;
    policy_.move_sigma = config.calibration.moveThresholdSigma;
    policy_.tighten_ratio = config.calibration.tightenRatio;
    policy_.settle = config.calibration.settleS;
}

std::filesystem::path CalibrationKeeper::databasePath(const NodeConfig& config)
{
    return core::paths::expand(config.calibration.database);
}

bool CalibrationKeeper::open(const std::filesystem::path& path)
{
    auto store = calibration_store::Store::open(path);
    if (!store)
    {
        report_.store_open = false;
        report_.store_error = store.error().message;
        return false;
    }
    store_ = std::move(*store);
    report_.store_open = true;
    report_.store_error.clear();
    if (const auto& rec = store_->recovered())
    {
        report_.store_recovered = rec->moved_to.string();
        SPDLOG_ERROR("[calibration] {} was damaged ({}); moved to {} and started afresh", path.string(), rec->reason,
                     rec->moved_to.string());
    }
    return true;
}

void CalibrationKeeper::seed(ve::Estimator& estimator)
{
    if (!store_) return;
    ve::CalibrationSet prior = ve::configuredCalibration(config_);
    bool any = false;
    double mag_evidence = 0.0;
    for (ve::CalibrationGroup g : ve::kCalibrationGroups)
    {
        const std::string hash = ve::hashHex(ve::priorHash(g, config_));
        std::vector<std::string> skipped;
        const auto row = store_->latest(
            ve::groupName(g), hash, ve::modelVersion(g),
            [g](const calibration_store::Row& r) -> std::optional<std::string> {
                std::string why;
                if (!fromRow(g, r, &why)) return why;
                return std::nullopt;
            },
            &skipped);
        for (const auto& s : skipped) SPDLOG_WARN("[calibration] {}: skipped {}", ve::groupName(g), s);
        report_.rows_skipped += static_cast<std::uint32_t>(skipped.size());
        if (!row)
        {
            SPDLOG_WARN("[calibration] reading {}: {}", ve::groupName(g), row.error().message);
            continue;
        }
        if (!*row)
        {
            SPDLOG_INFO("[calibration] {}: nothing learned against these priors yet ({}); using the config",
                        ve::groupName(g), hash);
            continue;
        }
        const auto stored = fromRow(g, **row, nullptr);
        const std::size_t i = indexOf(g);
        loaded_[i] = *stored;
        // A row from an earlier session: its time is not on this clock.
        last_[i] = ve::LastWritten{*stored, std::nullopt};
        ve::apply(prior, ve::loadPrior(*stored, config_, inflation_));
        report_.from_database[i] = true;
        if (g == ve::CalibrationGroup::magnetometer) mag_evidence = (**row).evidence_s;
        any = true;
        SPDLOG_INFO("[calibration] {}: from row {} ({}), {}", ve::groupName(g), (**row).id, (**row).reason,
                    (**row).summary);
    }
    // A stored magnetometer gives a heading on its own only if it was learned
    // for as long as this session's own would have to be.
    if (any) estimator.seedCalibration(prior, mag_evidence >= config_.mag_trust_after);
}

void CalibrationKeeper::tick(const ve::Estimator& estimator, bool shutdown)
{
    const auto& status = estimator.status();
    const auto state = estimator.keyframeState();
    const auto& current = estimator.calibration();
    if (!status.initialized || !state || !current)
    {
        started_.reset();
        return;
    }
    const double now = state->gps_time;
    if (!first_) first_ = now;
    // Settling counts from the latest (re)start of the estimator.
    if (!started_ || status.resets != seen_resets_)
    {
        started_ = now;
        seen_resets_ = status.resets;
    }

    std::vector<calibration_store::Row> rows;
    std::vector<std::pair<std::size_t, ve::GroupEstimate>> written;
    for (ve::CalibrationGroup g : ve::kCalibrationGroups)
    {
        const std::size_t i = indexOf(g);
        const ve::GroupEstimate estimate = ve::extract(*current, g);
        if (loaded_[i])
        {
            report_.moved_by[i] = ve::distanceFrom(*loaded_[i], estimate);
            if (report_.moved_by[i] > moved_sigma_ && !report_.moved[i])
            {
                report_.moved[i] = true;
                SPDLOG_WARN("[calibration] {} is {:.1f} sigma from what the last session learned: has it moved?",
                            ve::groupName(g), report_.moved_by[i]);
            }
        }
        if (!store_) continue;

        ve::WriteContext context;
        context.now = now;
        context.valid = state->valid;
        context.settled = now - *started_;
        context.shutdown = shutdown;
        const double evidence = evidenceFor(g, status, now - *first_);
        context.evidence = evidence;
        const auto reason = ve::decideWrite(policy_, estimate, last_[i], context);
        if (!reason) continue;

        calibration_store::Row row;
        row.written_at_ns = wall_();
        row.session = session_;
        row.group = std::string(ve::groupName(g));
        row.prior_hash = ve::hashHex(ve::priorHash(g, config_));
        row.model_version = ve::modelVersion(g);
        row.mean = toVector(estimate.mean);
        row.cov = toRowMajor(estimate.cov);
        row.summary = ve::summary(estimate);
        if (g == ve::CalibrationGroup::mounting)
            row.summary += fmt::format("; from {:.0f} s of straights and {} stops", status.mount_straight_s,
                                       status.mount_level_stops);
        else if (g == ve::CalibrationGroup::magnetometer)
            row.summary += fmt::format("; from {:.0f} s against a dual-antenna heading", evidence);
        else if (g == ve::CalibrationGroup::baro_airflow)
            row.summary += fmt::format("; from {:.0f} s moving", evidence);
        row.reason = std::string(ve::reasonName(*reason));
        row.evidence_s = evidence;
        row.drive_s = now - *first_;
        rows.push_back(std::move(row));
        written.push_back({i, estimate});
    }
    if (rows.empty()) return;

    // One transaction: the groups learned at this moment are kept together or
    // not at all, even across a power cut.
    const auto ids = store_->append(std::span<const calibration_store::Row>(rows));
    if (!ids)
    {
        SPDLOG_WARN("[calibration] writing {} rows failed: {}", rows.size(), ids.error().message);
        return;
    }
    for (std::size_t k = 0; k < rows.size(); ++k)
    {
        last_[written[k].first] = ve::LastWritten{written[k].second, now};
        ++report_.rows_written;
        SPDLOG_INFO("[calibration] {}: wrote row {} ({}): {}", rows[k].group, (*ids)[k], rows[k].reason,
                    rows[k].summary);
    }
}

}  // namespace state_estimator
