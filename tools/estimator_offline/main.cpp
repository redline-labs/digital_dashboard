// SPDX-License-Identifier: GPL-3.0-or-later
//
// estimator_offline: the state estimator over a recorded drive, both ways.
//
// Replays a bag through the same pipeline the live node runs -- decode,
// record pairing, the fixed-lag smoother -- and, from the keyframes that
// produced, solves the whole drive at once. Writes a new bag holding every
// input message plus:
//
//   estimator/fls     the fixed-lag estimate at IMU rate: what the car saw
//   estimator/batch   the whole-drive smoothed estimate, one per keyframe
//
// both as VehicleState, so a scope workspace can lay the two over each other
// and over sim/truth when the input came from estimator_sim.

#include <spdlog/spdlog.h>

#include <cxxopts.hpp>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "bag/reader.h"
#include "bag/writer.h"
#include "core/core.h"
#include "node_config.h"
#include "pipeline.h"
#include "state_fields.h"
#include "vehicle_estimator/offline.h"
#include "vehicle_state.capnp.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{

std::uint64_t nanos(double s)
{
    return static_cast<std::uint64_t>(std::llround(s * 1e9));
}

bool writeState(bag::BagWriter& w, const char* key, const vehicle_estimator::VehicleState& s, double host)
{
    ::capnp::MallocMessageBuilder b;
    state_estimator::fill(b.initRoot<::VehicleState>(), s);
    const auto words = ::capnp::messageToFlatArray(b);
    const auto bytes = words.asBytes();
    return w.write(key, "VehicleState", std::span<const std::uint8_t>(bytes.begin(), bytes.size()), nanos(host),
                   std::nullopt, "estimator_offline");
}

}  // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "estimator_offline"});
    cxxopts::Options options("estimator_offline", "Fixed-lag and whole-drive state estimates over a bag");
    options.add_options()("i,in", "Input bag directory", cxxopts::value<std::string>())(
        "o,out", "Output bag directory", cxxopts::value<std::string>())(
        "c,config", "Estimator config",
        cxxopts::value<std::string>()->default_value("configs/state_estimator/state_estimator.yaml"))(
        "time-offset", "Override imu.time_offset_s", cxxopts::value<double>())("h,help", "Help");
    cxxopts::ParseResult args;
    try
    {
        args = options.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        return 2;
    }
    if (args.count("help"))
    {
        fmt::print("{}\n", options.help());
        return 0;
    }
    if (!args.count("in") || !args.count("out"))
    {
        SPDLOG_ERROR("--in and --out are required");
        return 2;
    }

    state_estimator::NodeConfig config;
    if (!state_estimator::load_node_config(args["config"].as<std::string>(), config)) return 1;
    if (args.count("time-offset")) config.imuTimeOffsetS = args["time-offset"].as<double>();

    bag::BagReader reader(args["in"].as<std::string>());
    if (!reader.isValid())
    {
        for (const auto& p : reader.problems()) SPDLOG_ERROR("{}", p);
        return 1;
    }
    bag::WriterOptions wo;
    wo.recorder = "estimator_offline";
    bag::BagWriter writer(args["out"].as<std::string>(), wo);
    if (!writer.isValid())
    {
        SPDLOG_ERROR("cannot write {}", args["out"].as<std::string>());
        return 1;
    }

    state_estimator::Pipeline pipeline(config);
    vehicle_estimator::OfflineSmoother offline(state_estimator::estimatorConfig(config));
    pipeline.estimator().setKeyframeSink([&](const vehicle_estimator::KeyframeRecord& r) { offline.add(r); });

    std::vector<double> host_minus_gps;
    std::size_t fls = 0;
    bool ok = reader.forEach([&](const bag::BagMessage& m) {
        // Carried through unchanged: the output is the input plus estimates.
        if (!writer.write(m.key, m.schema, m.payload, m.log_time_ns, m.publish_time_ns, "")) return false;
        const double arrival = static_cast<double>(m.log_time_ns) * 1e-9;
        pipeline.onMessage(m.schema, m.payload, arrival);
        for (const auto& s : pipeline.advance(arrival))
        {
            if (!writeState(writer, "estimator/fls", s, arrival)) return false;
            if (fls++ % 50 == 0) host_minus_gps.push_back(arrival - s.gps_time);
        }
        return true;
    });
    if (!ok)
    {
        SPDLOG_ERROR("write failed");
        return 1;
    }
    const auto& st = pipeline.estimator().status();
    SPDLOG_INFO("fixed-lag: {} keyframes, {} states, {} resets, {} malformed messages", st.keyframes, fls, st.resets,
                pipeline.malformed());
    if (host_minus_gps.empty())
    {
        SPDLOG_ERROR("the estimator never started: no dual-antenna heading, or no IMU");
        writer.close();
        return 1;
    }

    const auto result = offline.solve();
    SPDLOG_INFO("batch: {} keyframes, {} iterations, cost {:.4g} -> {:.4g} ({}), {} refused", result.states.size(),
                result.report.iterations, result.report.initial_cost, result.report.final_cost,
                result.report.stop_reason, result.refused);
    // The batch states carry GPS time; the bag's clock is the host's. The
    // fixed-lag output pairs the two, so its median offset places them.
    std::nth_element(host_minus_gps.begin(), host_minus_gps.begin() + static_cast<std::ptrdiff_t>(host_minus_gps.size() / 2),
                     host_minus_gps.end());
    const double offset = host_minus_gps[host_minus_gps.size() / 2];
    for (const auto& s : result.states)
        if (!writeState(writer, "estimator/batch", s, s.gps_time + offset)) return 1;
    if (!writer.close()) return 1;
    SPDLOG_INFO("wrote {}", args["out"].as<std::string>());
    return result.report.converged ? 0 : 1;
}
