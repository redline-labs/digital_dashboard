// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the car learned about itself, carried from one session to the next,
// end to end: a simulated drive as the bridges' bytes, through Pipeline and
// the estimator, with CalibrationKeeper writing to a real SQLite file between
// sessions.
//
//   1. First drive, mounting 2 deg out in yaw: rows are written, no two of a
//      group closer than the interval unless the last is the shutdown one.
//   2. Same config again: it starts from what drive 1 learned, so the first
//      straight already reads the right slip -- which drive 1's did not.
//   3. The lever arm re-measured: the lever arm and boresight rows no longer
//      apply, the mounting row still does, and nothing old was deleted.
//   4. A file that is not a database: refused, and the drive runs on the
//      config as it would with no store at all.
//   5. The IMU knocked between sessions: the mounting is flagged as moved.
//   6. The store damaged between sessions: moved aside, kept, begun afresh.
//   7. Parked with no GNSS at power-on: session 1's magnetometer gives a
//      MAGNETIC heading before any fix -- and the barometer's offset, being
//      weather, was never stored to be given.

#include "calibration_keeper.h"
#include "pipeline.h"
#include "sim_bus.h"

#include "vehicle_estimator/calibration.h"
#include "vehicle_estimator/magnetic_reference.h"

#include "geodesy/geodetic.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <unistd.h>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

constexpr double kDeg = std::numbers::pi / 180.0;
namespace sim = vehicle_estimator::sim;
namespace ve = vehicle_estimator;
using state_estimator::CalibrationKeeper;

struct TempDir
{
    std::filesystem::path path;
    TempDir()
        : path(std::filesystem::temp_directory_path() / ("state_estimator_test_calibration_" + std::to_string(::getpid())))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

sim::SensorModel sensors(unsigned seed)
{
    sim::SensorModel s;
    s.seed = seed;
    return s;
}

// The node's config for a simulated car, the mounting stated `yaw_error`
// out in yaw. The policy's clocks are shortened to fit a drive of a minute
// and a half.
state_estimator::NodeConfig configFor(const sim::SensorModel& s, double yaw_error)
{
    state_estimator::NodeConfig c;
    // The simulator's default mounting is a half turn about x: roll 180.
    c.imuToBodyRpyDeg = Eigen::Vector3d(180.0, 0.0, yaw_error / kDeg);
    c.imuToBodySigmaDeg = Eigen::Vector3d(2.0, 2.0, 3.0);
    c.leverArmM = s.lever_arm;
    c.antenna2LeverArmM = s.antenna2_lever_arm;
    c.referencePointM = s.reference_point;
    c.imuTimeOffsetS = s.gnss_latency - s.imu_latency;
    c.calibration.settleS = 15.0;
    c.calibration.minWriteIntervalS = 30.0;
    return c;
}

struct Session
{
    state_estimator::CalibrationReport report;
    double first_mount_yaw_error = 0.0;  // at the first keyframe with a calibration
    double last_mount_yaw_error = 0.0;
    double launch_slip_error = 0.0;      // worst, on the launch straight before the gate has held
    bool initialized = false;
};

// Mounting yaw error about the body's down axis.
double mountYawError(const ve::Estimator& est, const Eigen::Matrix3d& truth)
{
    const Eigen::AngleAxisd d(est.calibration()->mounting.toRotationMatrix() * truth.transpose());
    return (d.angle() * d.axis()).z();
}

Session drive(const state_estimator::NodeConfig& config, const sim::Scenario& sc, const std::filesystem::path& db,
              const std::string& name)
{
    Session out;
    state_estimator::Pipeline pipeline(config);
    // Rows stamped with the drive's own clock, so the interval can be checked.
    double wall = 0.0;
    CalibrationKeeper keeper(config, name, [&wall] { return static_cast<std::int64_t>(std::llround(wall * 1e9)); });
    if (keeper.open(db)) keeper.seed(pipeline.estimator());

    std::uint64_t seen = 0;
    bool first = true;
    const Eigen::Matrix3d truth = sc.sensors().R_b_i;
    for (const auto& m : state_estimator::toBus(sc))
    {
        pipeline.onMessage(m.schema, m.payload, m.arrival);
        for (const auto& s : pipeline.advance(m.arrival))
        {
            const double t = sc.toScenarioTime(s.gps_time);
            if (t < 8.6 || t > 10.0 || !s.sideslip_valid) continue;
            const auto tr = sc.truth(t);
            if (tr.v_body.x() < 15.0) continue;
            out.launch_slip_error = std::max(out.launch_slip_error, std::fabs(std::remainder(s.sideslip - tr.sideslip, 2.0 * std::numbers::pi)));
        }
        const auto& est = pipeline.estimator();
        if (est.status().keyframes == seen) continue;
        seen = est.status().keyframes;
        if (!est.calibration()) continue;
        if (first)
        {
            out.first_mount_yaw_error = mountYawError(est, truth);
            first = false;
        }
        if (const auto k = est.keyframeState()) wall = sc.toScenarioTime(k->gps_time);
        keeper.tick(est);
    }
    keeper.tick(pipeline.estimator(), true);
    out.initialized = pipeline.estimator().status().initialized;
    if (pipeline.estimator().calibration()) out.last_mount_yaw_error = mountYawError(pipeline.estimator(), truth);
    out.report = keeper.report();
    SPDLOG_INFO("{}: store {}, from database [{} {} {} {} {}], moved [{} {} {}] ({:.1f} {:.1f} {:.1f} sigma), {} rows "
                "written; mounting yaw error {:.3f} deg at the start, {:.3f} deg at the end; launch slip error {:.3f} "
                "deg",
                name, out.report.store_open ? "open" : out.report.store_error, out.report.from_database[0],
                out.report.from_database[1], out.report.from_database[2], out.report.from_database[3],
                out.report.from_database[4], out.report.moved[0], out.report.moved[1],
                out.report.moved[2], out.report.moved_by[0], out.report.moved_by[1], out.report.moved_by[2],
                out.report.rows_written, out.first_mount_yaw_error / kDeg, out.last_mount_yaw_error / kDeg,
                out.launch_slip_error / kDeg);
    return out;
}

// Power on parked with no GNSS for `outage` s: the states before the first
// fix, and how far their heading is from true minus the declination.
struct ColdSession
{
    bool anchored = false, magnetic = false;
    ve::HeadingSource source = ve::HeadingSource::none;
    double magnetic_heading_error = 0.0;
};

ColdSession coldSession(const state_estimator::NodeConfig& config, const sim::Scenario& sc, double outage,
                        const std::filesystem::path& db)
{
    ColdSession out;
    state_estimator::Pipeline pipeline(config);
    CalibrationKeeper keeper(config, "cold", [] { return std::int64_t{0}; });
    if (keeper.open(db)) keeper.seed(pipeline.estimator());
    const auto p = sc.truth(0.0).p_e;
    const auto llh = geodesy::ecefToLlh(csym::Vector3<double>{p.x(), p.y(), p.z()});
    const double D = ve::magneticField(llh.lat, llh.lon, llh.h, sc.sensors().gps_epoch).declination;
    for (const auto& m : state_estimator::toBus(sc))
    {
        pipeline.onMessage(m.schema, m.payload, m.arrival);
        const double t = m.arrival - sc.sensors().host_epoch;
        for (const auto& st : pipeline.advance(m.arrival))
        {
            if (t < 10.0 || t > outage - 1.0) continue;
            out.anchored = out.anchored || pipeline.estimator().status().anchored;
            out.magnetic = st.heading_magnetic;
            out.source = st.heading_source;
            const double e = std::remainder(st.yaw - (sc.truth(t).yaw - D), 2.0 * std::numbers::pi);
            out.magnetic_heading_error = std::max(out.magnetic_heading_error, std::fabs(e));
        }
    }
    SPDLOG_INFO("cold session: anchored {}, magnetic {}, magnetic heading error {:.2f} deg", out.anchored,
                out.magnetic, out.magnetic_heading_error / kDeg);
    return out;
}

sim::TrackParams shortTrack()
{
    sim::TrackParams p;
    p.laps = 2;
    return p;
}

}  // namespace

int main()
{
    TempDir dir;
    const auto db = dir.path / "state_estimator" / "calibration.sqlite";

    // ---- 1. first drive ------------------------------------------------------------
    const auto s1 = sensors(31);
    const sim::Scenario sc1(sim::track(shortTrack()), s1);
    const auto config = configFor(s1, 2.0 * kDeg);
    const Session first = drive(config, sc1, db, "session 1");
    check(first.initialized, "session 1 ran");
    check(first.report.store_open, "the store opened, making its directory");
    check(!first.report.from_database[0] && !first.report.from_database[1] && !first.report.from_database[2],
          "an empty store seeds nothing");
    check(std::fabs(first.launch_slip_error) > 1.5 * kDeg, "the first launch reads the 2 deg mounting as slip");
    check(std::fabs(first.last_mount_yaw_error) < 0.3 * kDeg, "and the drive learns it");

    {
        auto store = calibration_store::Store::open(db);
        check(store.has_value(), "the store reopens after the session");
        for (ve::CalibrationGroup g : ve::kCalibrationGroups)
        {
            const auto rows = store->history(ve::groupName(g));
            check(rows && !rows->empty(), std::string(ve::groupName(g)) + " has rows");
            if (!rows) continue;
            for (std::size_t i = 1; i < rows->size(); ++i)
            {
                const double gap = static_cast<double>((*rows)[i].written_at_ns - (*rows)[i - 1].written_at_ns) * 1e-9;
                const bool shutdown = (*rows)[i].reason.starts_with("shutdown");
                check(shutdown || gap >= 30.0 - 1e-6,
                      fmt::format("{} rows {} s apart, inside the interval, and not at shutdown", ve::groupName(g), gap));
            }
            if (g == ve::CalibrationGroup::magnetometer && !rows->empty())
                check(rows->back().evidence_s > 30.0,
                      fmt::format("the magnetometer was learned against the antennas ({:.0f} s)", rows->back().evidence_s));
            for (const auto& r : *rows) SPDLOG_INFO("  {} row {}: {} at {:.0f} s: {}", r.group, r.id, r.reason,
                                                    static_cast<double>(r.written_at_ns) * 1e-9, r.summary);
        }
    }

    {
        // No row of any other group: in particular no barometric offset,
        // which is the day's weather and must be learned afresh.
        sqlite3* raw = nullptr;
        sqlite3_open_v2(db.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(raw, "SELECT DISTINCT grp FROM calibration", -1, &st, nullptr);
        std::size_t groups = 0;
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            const std::string grp = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            check(ve::groupFromName(grp).has_value(), "a stored group that is not a calibration group: " + grp);
            check(grp.find("offset") == std::string::npos, "the barometer's offset is never stored");
            ++groups;
        }
        sqlite3_finalize(st);
        sqlite3_close(raw);
        check(groups == ve::kCalibrationGroups.size(), "every group, and only those, has rows");
    }

    // ---- 2. the same car, the same config -------------------------------------------
    const auto s2 = sensors(32);
    const sim::Scenario sc2(sim::track(shortTrack()), s2);
    const Session second = drive(config, sc2, db, "session 2");
    check(second.report.from_database[0] && second.report.from_database[1] && second.report.from_database[2] &&
              second.report.from_database[3] && second.report.from_database[4],
          "every group starts from what session 1 learned");
    check(std::fabs(second.first_mount_yaw_error) < 0.3 * kDeg,
          "so the mounting is right from the first keyframe, not 2 deg out");
    check(second.launch_slip_error < 0.5 * kDeg, "and the first launch already reads true");
    check(!second.report.moved[0] && !second.report.moved[1] && !second.report.moved[2], "nothing has moved");

    // ---- 3. the lever arm re-measured -------------------------------------------------
    auto remeasured = config;
    remeasured.leverArmM.x() += 0.01;
    const Session third = drive(remeasured, sc2, db, "session 3");
    check(third.report.from_database[0], "the mounting row still applies");
    check(!third.report.from_database[1] && !third.report.from_database[2],
          "the lever arm and boresight rows do not: their priors changed");
    {
        auto store = calibration_store::Store::open(db);
        const auto rows = store->history("lever_arm");
        const std::string old_hash = ve::hashHex(ve::priorHash(ve::CalibrationGroup::lever_arm, state_estimator::estimatorConfig(config)));
        const std::string new_hash = ve::hashHex(ve::priorHash(ve::CalibrationGroup::lever_arm, state_estimator::estimatorConfig(remeasured)));
        std::size_t old_rows = 0, new_rows = 0;
        for (const auto& r : *rows)
        {
            old_rows += r.prior_hash == old_hash;
            new_rows += r.prior_hash == new_hash;
        }
        check(old_rows > 0, "the old lever-arm rows are still in the history");
        check(new_rows > 0, "and the new ones carry the new hash");
    }

    // ---- 7. parked at power-on, no GNSS ---------------------------------------------
    // Before the lever arm was re-measured in session 3, so run on session 3's
    // config: the magnetometer's rows do not care about the lever arm.
    {
        auto s7 = sensors(37);
        s7.outages = {{0.0, 40.0}};
        const sim::Scenario sc7(sim::scripted({{45.0, 0.0}, {6.0, 15.0}, {10.0, 15.0}}), s7);
        const ColdSession cold = coldSession(remeasured, sc7, 40.0, db);
        check(cold.anchored, "no GNSS at power-on: an anchored start");
        check(cold.magnetic && cold.source == ve::HeadingSource::magnetometer,
              "with a magnetic heading from the stored magnetometer");
        check(cold.magnetic_heading_error < 3.0 * kDeg, "to 3 degrees");

        // And from an empty store, the same drive has no heading to give.
        const ColdSession fresh = coldSession(remeasured, sc7, 40.0, dir.path / "empty.sqlite");
        check(fresh.anchored && fresh.source == ve::HeadingSource::none && !fresh.magnetic,
              "an unlearned magnetometer gives no heading at all");
    }

    // ---- 8. a drive that loses the antennas' heading early ----------------------------
    // The magnetometer is learned only against that heading: its row must say
    // how little it had, not how long the car drove, or the next cold start
    // would trust a calibration nothing taught.
    {
        auto s8 = sensors(38);
        s8.attitude_outages = {{20.0, 1e9}};
        const sim::Scenario sc8(sim::track(shortTrack()), s8);
        const auto db8 = dir.path / "short_heading.sqlite";
        drive(config, sc8, db8, "session 8");
        auto store = calibration_store::Store::open(db8);
        const auto mag = store->history("magnetometer");
        const auto lever_rows = store->history("lever_arm");
        check(lever_rows && !lever_rows->empty() && lever_rows->back().drive_s > 60.0, "a drive of over a minute");
        double evidence = 0.0;
        if (mag && !mag->empty()) evidence = mag->back().evidence_s;
        check(evidence < 20.0, fmt::format("but the magnetometer's row counts only its {:.0f} s of heading", evidence));
    }

    // ---- 4. a file that is not a database ---------------------------------------------
    const auto notes = dir.path / "notes.txt";
    {
        std::ofstream f(notes);
        f << "not a database\n" << std::string(5000, 'x');
    }
    const auto s4 = sensors(34);
    sim::TrackParams tiny;
    tiny.laps = 0;
    const sim::Scenario sc4(sim::track(tiny), s4);
    const Session fourth = drive(config, sc4, notes, "session 4");
    check(!fourth.report.store_open && !fourth.report.store_error.empty(), "a file that is not ours is refused, with a reason");
    check(fourth.initialized, "and the drive runs on the config regardless");

    // ---- 5. the IMU knocked between sessions -------------------------------------------
    auto s5 = sensors(35);
    s5.R_b_i = Eigen::AngleAxisd(1.5 * kDeg, Eigen::Vector3d::UnitZ()).toRotationMatrix() * s5.R_b_i;
    const sim::Scenario sc5(sim::track(shortTrack()), s5);
    const Session fifth = drive(config, sc5, db, "session 5");
    check(fifth.report.from_database[0], "session 5 starts from the stored mounting");
    check(fifth.report.moved[0], "and notices the IMU has moved since");
    check(!fifth.report.moved[1], "while the lever arm has not");

    // ---- 6. the store damaged between sessions ----------------------------------------
    // Its calibration table's root page scribbled over, as a power cut on
    // storage that lied about a flush could leave it: the node moves it aside,
    // starts a fresh history, and says so, rather than running on the config
    // until someone comes to look.
    {
        sqlite3* raw = nullptr;
        sqlite3_open(db.string().c_str(), &raw);
        sqlite3_exec(raw, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
        sqlite3_int64 root = 0, page = 0;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(raw, "SELECT rootpage FROM sqlite_master WHERE name='calibration'", -1, &st, nullptr);
        if (sqlite3_step(st) == SQLITE_ROW) root = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        sqlite3_prepare_v2(raw, "PRAGMA page_size", -1, &st, nullptr);
        if (sqlite3_step(st) == SQLITE_ROW) page = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        sqlite3_close(raw);
        std::fstream f(db, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>((root - 1) * page));
        const std::string junk(static_cast<std::size_t>(page), 'Z');
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    const Session sixth = drive(config, sc4, db, "session 6");
    check(sixth.report.store_open, "a damaged store is recovered, not refused");
    check(!sixth.report.store_recovered.empty() && std::filesystem::exists(sixth.report.store_recovered),
          "and the report names where the damaged one was kept");
    check(!sixth.report.from_database[0], "the fresh store has nothing to seed from");

    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("calibration persistence: all passed");
    return 0;
}
