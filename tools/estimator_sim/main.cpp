// SPDX-License-Identifier: GPL-3.0-or-later
//
// estimator_sim: a simulated drive, recorded.
//
// Writes a bag holding what mti610_bridge and bd992_bridge would have put on
// the bus during the drive -- the same keys, schemas and arrival jitter --
// plus the truth on `sim/truth`, as a VehicleState so it plots against the
// estimator's output field by field. `bag play` it to drive the live node,
// or hand it to estimator_offline.

#include <spdlog/spdlog.h>

#include <cxxopts.hpp>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "bag/writer.h"
#include "core/core.h"
#include "sim_bus.h"
#include "state_fields.h"
#include "vehicle_state.capnp.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace sim = vehicle_estimator::sim;

namespace
{

std::unique_ptr<sim::VehicleMotion> motion(const std::string& name, double drive)
{
    if (name == "skidpad")
    {
        sim::SkidpadParams p;
        p.drive = drive;
        return sim::skidpad(p);
    }
    if (name == "spin")
    {
        sim::SkidpadParams p;
        p.drive = drive;
        p.spin = true;
        return sim::skidpad(p);
    }
    if (name == "figure8")
    {
        sim::FigureEightParams p;
        p.drive = drive;
        return sim::figureEight(p);
    }
    if (name == "parked") return sim::parked(drive);
    if (name == "track")
    {
        // Laps of straights and drifted corners, as many as fit the drive.
        sim::TrackParams p;
        p.laps = std::max(1, static_cast<int>(drive / (2.0 * (p.straight + p.corner))));
        return sim::track(p);
    }
    if (name == "stopandgo")
    {
        sim::StopAndGoParams p;
        p.stops = std::max(1, static_cast<int>(drive / (p.drive + p.brake + p.hold + 3.0)));
        return sim::stopAndGo(p);
    }
    return nullptr;
}

std::uint64_t nanos(double s)
{
    return static_cast<std::uint64_t>(std::llround(s * 1e9));
}

}  // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "estimator_sim"});
    cxxopts::Options options("estimator_sim", "Record a simulated drive for the state estimator");
    options.add_options()("o,out", "Bag directory to write", cxxopts::value<std::string>())(
        "scenario", "skidpad | spin | figure8 | parked | track | stopandgo", cxxopts::value<std::string>()->default_value("skidpad"))(
        "drive", "Seconds of driving after the start", cxxopts::value<double>()->default_value("40"))(
        "seed", "Noise seed", cxxopts::value<unsigned>()->default_value("1"))(
        "outage", "A GNSS outage, 'from:to' in seconds", cxxopts::value<std::string>())("h,help", "Help");
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
    if (!args.count("out"))
    {
        SPDLOG_ERROR("--out is required");
        return 2;
    }

    auto m = motion(args["scenario"].as<std::string>(), args["drive"].as<double>());
    if (!m)
    {
        SPDLOG_ERROR("unknown scenario '{}'", args["scenario"].as<std::string>());
        return 2;
    }
    sim::SensorModel sensors;
    sensors.seed = args["seed"].as<unsigned>();
    if (args.count("outage"))
    {
        const std::string o = args["outage"].as<std::string>();
        const auto colon = o.find(':');
        try
        {
            sensors.outages = {{std::stod(o.substr(0, colon)), std::stod(o.substr(colon + 1))}};
        }
        catch (const std::exception&)
        {
            SPDLOG_ERROR("--outage wants 'from:to'");
            return 2;
        }
    }
    const sim::Scenario sc(std::move(m), sensors);

    bag::WriterOptions wo;
    wo.recorder = "estimator_sim";
    bag::BagWriter writer(args["out"].as<std::string>(), wo);
    if (!writer.isValid())
    {
        SPDLOG_ERROR("cannot write {}", args["out"].as<std::string>());
        return 1;
    }

    std::size_t n = 0;
    for (const auto& msg : state_estimator::toBus(sc))
    {
        if (!writer.write(msg.key, msg.schema, msg.payload, nanos(msg.arrival), std::nullopt, "estimator_sim"))
            return 1;
        ++n;
    }
    // Truth at 100 Hz, stamped at the moment it describes on the host clock.
    for (double t = 0.01; t < sc.duration() - 0.01; t += 0.01)
    {
        ::capnp::MallocMessageBuilder b;
        state_estimator::fill(b.initRoot<::VehicleState>(), state_estimator::truthState(sc, t));
        const auto words = ::capnp::messageToFlatArray(b);
        const auto bytes = words.asBytes();
        if (!writer.write("sim/truth", "VehicleState", std::span<const std::uint8_t>(bytes.begin(), bytes.size()),
                          nanos(sensors.host_epoch + t), std::nullopt, "estimator_sim"))
            return 1;
        ++n;
    }
    if (!writer.close()) return 1;
    SPDLOG_INFO("wrote {} messages, {:.0f} s of '{}', to {}", n, sc.duration(), args["scenario"].as<std::string>(),
                args["out"].as<std::string>());
    SPDLOG_INFO("the estimator needs imu.time_offset_s: {:.3f} for this recording", sensors.gnss_latency - sensors.imu_latency);
    return 0;
}
