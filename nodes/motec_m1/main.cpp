#include "dbc_motec_m1_rev3_parser.h"
#include "motec_m1_messages.h"

#include "cli/interrupt.h"
#include "node_health/reporter.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"
#include "motec_m1.capnp.h"

#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>
#include <thread>
#include <chrono>

using namespace dbc_motec_m1_rev3;

static void publishEngineAir(const M1_GEN_0x640_t& m, pub_sub::ZenohPublisher<MotecM1EngineAir>& pub)
{
    motec_m1::fillEngineAir(m, pub.fields());
    pub.put();
}

static void publishFuelStatus(const M1_GEN_0x641_t& m, pub_sub::ZenohPublisher<MotecM1FuelStatus>& pub)
{
    motec_m1::fillFuelStatus(m, pub.fields());
    pub.put();
}

static void publishTemperatures(const M1_GEN_0x649_t& m, pub_sub::ZenohPublisher<MotecM1Temperatures>& pub)
{
    motec_m1::fillTemperatures(m, pub.fields());
    pub.put();
}

static void publishExhaust(const M1_GEN_0x651_t& m, pub_sub::ZenohPublisher<MotecM1Exhaust>& pub)
{
    motec_m1::fillExhaust(m, pub.fields());
    pub.put();
}

static void publishThrottleTiming(const M1_GEN_0x642_t& m, pub_sub::ZenohPublisher<MotecM1ThrottleTiming>& pub)
{
    motec_m1::fillThrottleTiming(m, pub.fields());
    pub.put();
}

static void publishCutsAndOilPressure(const M1_GEN_0x644_t& m, pub_sub::ZenohPublisher<MotecM1CutsAndOilPressure>& pub)
{
    motec_m1::fillCutsAndOilPressure(m, pub.fields());
    pub.put();
}

static void publishBoostStatus(const M1_GEN_0x645_t& m, pub_sub::ZenohPublisher<MotecM1BoostStatus>& pub)
{
    motec_m1::fillBoostStatus(m, pub.fields());
    pub.put();
}

static void publishInletCam(const M1_GEN_0x646_t& m, pub_sub::ZenohPublisher<MotecM1InletCam>& pub)
{
    motec_m1::fillInletCam(m, pub.fields());
    pub.put();
}

static void publishExhaustCam(const M1_GEN_0x647_t& m, pub_sub::ZenohPublisher<MotecM1ExhaustCam>& pub)
{
    motec_m1::fillExhaustCam(m, pub.fields());
    pub.put();
}

static void publishWheelSpeeds(const M1_GEN_0x648_t& m, pub_sub::ZenohPublisher<MotecM1WheelSpeeds>& pub)
{
    motec_m1::fillWheelSpeeds(m, pub.fields());
    pub.put();
}

static void publishEnvironment(const M1_GEN_0x64A_t& m, pub_sub::ZenohPublisher<MotecM1Environment>& pub)
{
    motec_m1::fillEnvironment(m, pub.fields());
    pub.put();
}

static void publishRuntimeWarnings(const M1_GEN_0x64C_t& m, pub_sub::ZenohPublisher<MotecM1RuntimeWarnings>& pub)
{
    motec_m1::fillRuntimeWarnings(m, pub.fields());
    pub.put();
}

static void publishStates(const M1_GEN_0x64D_t& m, pub_sub::ZenohPublisher<MotecM1States>& pub)
{
    motec_m1::fillStates(m, pub.fields());
    pub.put();
}

static void publishDiagnostics(const M1_GEN_0x64E_t& m, pub_sub::ZenohPublisher<MotecM1Diagnostics>& pub)
{
    motec_m1::fillDiagnostics(m, pub.fields());
    pub.put();
}

static void publishTotals(const M1_GEN_0x64F_t& m, pub_sub::ZenohPublisher<MotecM1Totals>& pub)
{
    motec_m1::fillTotals(m, pub.fields());
    pub.put();
}

static void publishDriverControls(const M1_GEN_0x650_t& m, pub_sub::ZenohPublisher<MotecM1DriverControls>& pub)
{
    motec_m1::fillDriverControls(m, pub.fields());
    pub.put();
}

static void publishFuelSecondary(const M1_GEN_0x652_t& m, pub_sub::ZenohPublisher<MotecM1FuelSecondary>& pub)
{
    motec_m1::fillFuelSecondary(m, pub.fields());
    pub.put();
}

static void publishPressures(const M1_GEN_0x655_t& m, pub_sub::ZenohPublisher<MotecM1Pressures>& pub)
{
    motec_m1::fillPressures(m, pub.fields());
    pub.put();
}

static void publishFlows(const M1_GEN_0x656_t& m, pub_sub::ZenohPublisher<MotecM1Flows>& pub)
{
    motec_m1::fillFlows(m, pub.fields());
    pub.put();
}

static void publishInjectorPressures(const M1_GEN_0x657_t& m, pub_sub::ZenohPublisher<MotecM1InjectorPressures>& pub)
{
    motec_m1::fillInjectorPressures(m, pub.fields());
    pub.put();
}

static void publishVehicleDynamics(const M1_GEN_0x658_t& m, pub_sub::ZenohPublisher<MotecM1VehicleDynamics>& pub)
{
    motec_m1::fillVehicleDynamics(m, pub.fields());
    pub.put();
}

static void publishFuelDirectAll(const M1_GEN_0x653_t& b1, const M1_GEN_0x654_t& b2, pub_sub::ZenohPublisher<MotecM1FuelDirectAll>& pub)
{
    motec_m1::fillFuelDirectAll(b1, b2, pub.fields());
    pub.put();
}

static void publishTurboBoth(const M1_GEN_0x6A6_t& b1, const M1_GEN_0x6A7_t& b2, pub_sub::ZenohPublisher<MotecM1Turbo>& pub)
{
    motec_m1::fillTurboBoth(b1, b2, pub.fields());
    pub.put();
}

static void publishKnock1to12(const M1_GEN_0x643_t& k1, const M1_GEN_0x659_t& k2, pub_sub::ZenohPublisher<MotecM1KnockLevels1to12>& pub)
{
    motec_m1::fillKnock1to12(k1, k2, pub.fields());
    pub.put();
}

static void publishIgnTrim1to12(const M1_GEN_0x64B_t& t1, const M1_GEN_0x65A_t& t2, pub_sub::ZenohPublisher<MotecM1IgnitionTrim1to12>& pub)
{
    motec_m1::fillIgnTrim1to12(t1, t2, pub.fields());
    pub.put();
}

static void publishLapTiming(const M1_GEN_0x65B_t& m, pub_sub::ZenohPublisher<MotecM1LapTiming>& pub)
{
    motec_m1::fillLapTiming(m, pub.fields());
    pub.put();
}

static void publishDiffAndRotary(const M1_GEN_0x65C_t& m, pub_sub::ZenohPublisher<MotecM1DiffAndRotary>& pub)
{
    motec_m1::fillDiffAndRotary(m, pub.fields());
    pub.put();
}

static void publishBrakeTemperatures(const M1_GEN_0x65D_t& m, pub_sub::ZenohPublisher<MotecM1BrakeTemperatures>& pub)
{
    motec_m1::fillBrakeTemperatures(m, pub.fields());
    pub.put();
}

static void publishExhaustPressures(const M1_GEN_0x65E_t& m, pub_sub::ZenohPublisher<MotecM1ExhaustPressures>& pub)
{
    motec_m1::fillExhaustPressures(m, pub.fields());
    pub.put();
}

static void publishThresholdsAndLimits(const M1_GEN_0x65F_t& m, pub_sub::ZenohPublisher<MotecM1ThresholdsAndLimits>& pub)
{
    motec_m1::fillThresholdsAndLimits(m, pub.fields());
    pub.put();
}

static void publishAuxOutputs(const M1_GEN_0x6A0_t& m, pub_sub::ZenohPublisher<MotecM1AuxOutputs>& pub)
{
    motec_m1::fillAuxOutputs(m, pub.fields());
    pub.put();
}

static void publishAuxOutput5(const M1_GEN_0x6A1_t& m, pub_sub::ZenohPublisher<MotecM1AuxOutput5>& pub)
{
    motec_m1::fillAuxOutput5(m, pub.fields());
    pub.put();
}

int main(int argc, char** argv)
{
    cxxopts::Options options("motec_m1", "MoTeC M1 node");
    options.add_options()
        ("s,source", "Zenoh key carrying CAN frames",
            cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("p,prefix", "Zenoh key prefix for this node's topics",
            cxxopts::value<std::string>()->default_value("nodes/motec_m1"))
        ("debug", "Debug logging.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    // Logging is set up AFTER the parse, so --debug decides the level rather
    // than the level being forced before anyone can ask for anything else.
    core::setupLogging({.program = "motec_m1", .debug = result["debug"].as<bool>()});

    const std::string can_key = result["source"].as<std::string>();
    const std::string prefix = result["prefix"].as<std::string>();

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("motec_m1");

    // SIGINT and SIGTERM both set the flag the loop below polls.
    cli::installInterruptHandler();

    pub_sub::ZenohPublisher<MotecM1EngineAir> pubEngineAir(prefix + "/engine_air");
    pub_sub::ZenohPublisher<MotecM1FuelStatus> pubFuelStatus(prefix + "/fuel_status");
    pub_sub::ZenohPublisher<MotecM1Temperatures> pubTemps(prefix + "/temperatures");
    pub_sub::ZenohPublisher<MotecM1Exhaust> pubExhaust(prefix + "/exhaust");
    pub_sub::ZenohPublisher<MotecM1ThrottleTiming> pubThrottleTiming(prefix + "/throttle_timing");
    pub_sub::ZenohPublisher<MotecM1CutsAndOilPressure> pubCutsOil(prefix + "/cuts_oil_pressure");
    pub_sub::ZenohPublisher<MotecM1BoostStatus> pubBoost(prefix + "/boost_status");
    pub_sub::ZenohPublisher<MotecM1InletCam> pubInletCam(prefix + "/inlet_cam");
    pub_sub::ZenohPublisher<MotecM1ExhaustCam> pubExhaustCam(prefix + "/exhaust_cam");
    pub_sub::ZenohPublisher<MotecM1WheelSpeeds> pubWheelSpeeds(prefix + "/wheel_speeds");
    pub_sub::ZenohPublisher<MotecM1Environment> pubEnv(prefix + "/environment");
    pub_sub::ZenohPublisher<MotecM1RuntimeWarnings> pubWarnings(prefix + "/runtime_warnings");
    pub_sub::ZenohPublisher<MotecM1States> pubStates(prefix + "/states");
    pub_sub::ZenohPublisher<MotecM1Diagnostics> pubDiag(prefix + "/diagnostics");
    pub_sub::ZenohPublisher<MotecM1Totals> pubTotals(prefix + "/totals");
    pub_sub::ZenohPublisher<MotecM1DriverControls> pubDriver(prefix + "/driver_controls");
    pub_sub::ZenohPublisher<MotecM1FuelSecondary> pubFuelSec(prefix + "/fuel_secondary");
    pub_sub::ZenohPublisher<MotecM1FuelDirectAll> pubFuelDirectAll(prefix + "/fuel_direct_all");
    pub_sub::ZenohPublisher<MotecM1Pressures> pubPressures(prefix + "/pressures");
    pub_sub::ZenohPublisher<MotecM1Flows> pubFlows(prefix + "/flows");
    pub_sub::ZenohPublisher<MotecM1InjectorPressures> pubInjPress(prefix + "/injector_pressures");
    pub_sub::ZenohPublisher<MotecM1VehicleDynamics> pubVehDyn(prefix + "/vehicle_dynamics");
    pub_sub::ZenohPublisher<MotecM1LapTiming> pubLap(prefix + "/lap_timing");
    pub_sub::ZenohPublisher<MotecM1DiffAndRotary> pubDiffRot(prefix + "/diff_rotary");
    pub_sub::ZenohPublisher<MotecM1BrakeTemperatures> pubBrakeTemps(prefix + "/brake_temperatures");
    pub_sub::ZenohPublisher<MotecM1ExhaustPressures> pubExhPress(prefix + "/exhaust_pressures");
    pub_sub::ZenohPublisher<MotecM1ThresholdsAndLimits> pubThresh(prefix + "/thresholds_limits");
    pub_sub::ZenohPublisher<MotecM1AuxOutputs> pubAuxOuts(prefix + "/aux_outputs");
    pub_sub::ZenohPublisher<MotecM1AuxOutput5> pubAux5(prefix + "/aux_output5");
    pub_sub::ZenohPublisher<MotecM1Turbo> pubTurbo(prefix + "/turbo");

    pub_sub::ZenohPublisher<MotecM1KnockLevels1to12> pubKnock1to12(prefix + "/knock_levels_1_12");
    pub_sub::ZenohPublisher<MotecM1IgnitionTrim1to12> pubIgnTrim1to12(prefix + "/ignition_trim_1_12");

    dbc_motec_m1_rev3_parser parser;
    parser.on_M1_GEN_0x640([&](const M1_GEN_0x640_t& m){ publishEngineAir(m, pubEngineAir); });
    parser.on_M1_GEN_0x641([&](const M1_GEN_0x641_t& m){ publishFuelStatus(m, pubFuelStatus); });
    parser.on_M1_GEN_0x649([&](const M1_GEN_0x649_t& m){ publishTemperatures(m, pubTemps); });
    parser.on_M1_GEN_0x651([&](const M1_GEN_0x651_t& m){ publishExhaust(m, pubExhaust); });
    parser.on_M1_GEN_0x642([&](const M1_GEN_0x642_t& m){ publishThrottleTiming(m, pubThrottleTiming); });
    parser.on_M1_GEN_0x644([&](const M1_GEN_0x644_t& m){ publishCutsAndOilPressure(m, pubCutsOil); });
    parser.on_M1_GEN_0x645([&](const M1_GEN_0x645_t& m){ publishBoostStatus(m, pubBoost); });
    parser.on_M1_GEN_0x646([&](const M1_GEN_0x646_t& m){ publishInletCam(m, pubInletCam); });
    parser.on_M1_GEN_0x647([&](const M1_GEN_0x647_t& m){ publishExhaustCam(m, pubExhaustCam); });
    parser.on_M1_GEN_0x648([&](const M1_GEN_0x648_t& m){ publishWheelSpeeds(m, pubWheelSpeeds); });
    parser.on_M1_GEN_0x64A([&](const M1_GEN_0x64A_t& m){ publishEnvironment(m, pubEnv); });
    parser.on_M1_GEN_0x64C([&](const M1_GEN_0x64C_t& m){ publishRuntimeWarnings(m, pubWarnings); });
    parser.on_M1_GEN_0x64D([&](const M1_GEN_0x64D_t& m){ publishStates(m, pubStates); });
    parser.on_M1_GEN_0x64E([&](const M1_GEN_0x64E_t& m){ publishDiagnostics(m, pubDiag); });
    parser.on_M1_GEN_0x64F([&](const M1_GEN_0x64F_t& m){ publishTotals(m, pubTotals); });
    parser.on_M1_GEN_0x650([&](const M1_GEN_0x650_t& m){ publishDriverControls(m, pubDriver); });
    parser.on_M1_GEN_0x652([&](const M1_GEN_0x652_t& m){ publishFuelSecondary(m, pubFuelSec); });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x653,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x654
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishFuelDirectAll(db.M1_GEN_0x653, db.M1_GEN_0x654, pubFuelDirectAll);
    });
    parser.on_M1_GEN_0x655([&](const M1_GEN_0x655_t& m){ publishPressures(m, pubPressures); });
    parser.on_M1_GEN_0x656([&](const M1_GEN_0x656_t& m){ publishFlows(m, pubFlows); });
    parser.on_M1_GEN_0x657([&](const M1_GEN_0x657_t& m){ publishInjectorPressures(m, pubInjPress); });
    parser.on_M1_GEN_0x658([&](const M1_GEN_0x658_t& m){ publishVehicleDynamics(m, pubVehDyn); });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x643,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x659
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishKnock1to12(db.M1_GEN_0x643, db.M1_GEN_0x659, pubKnock1to12);
    });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x64B,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x65A
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishIgnTrim1to12(db.M1_GEN_0x64B, db.M1_GEN_0x65A, pubIgnTrim1to12);
    });
    parser.on_M1_GEN_0x65B([&](const M1_GEN_0x65B_t& m){ publishLapTiming(m, pubLap); });
    parser.on_M1_GEN_0x65C([&](const M1_GEN_0x65C_t& m){ publishDiffAndRotary(m, pubDiffRot); });
    parser.on_M1_GEN_0x65D([&](const M1_GEN_0x65D_t& m){ publishBrakeTemperatures(m, pubBrakeTemps); });
    parser.on_M1_GEN_0x65E([&](const M1_GEN_0x65E_t& m){ publishExhaustPressures(m, pubExhPress); });
    parser.on_M1_GEN_0x65F([&](const M1_GEN_0x65F_t& m){ publishThresholdsAndLimits(m, pubThresh); });
    parser.on_M1_GEN_0x6A0([&](const M1_GEN_0x6A0_t& m){ publishAuxOutputs(m, pubAuxOuts); });
    parser.on_M1_GEN_0x6A1([&](const M1_GEN_0x6A1_t& m){ publishAuxOutput5(m, pubAux5); });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x6A6,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x6A7
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishTurboBoth(db.M1_GEN_0x6A6, db.M1_GEN_0x6A7, pubTurbo);
    });

    // Declared before the subscriber, so the subscriber is destroyed first and
    // no callback can touch a check that has gone away.
    node_health::HealthReporter health("motec_m1");
    // Frames arriving at all, and frames this node could decode: a quiet bus
    // and a bus carrying nothing but other devices look identical otherwise.
    auto& frames_in = health.addActivityCheck("can_rx", std::chrono::seconds(1));
    auto& decoded = health.addActivityCheck("decoded", std::chrono::seconds(2));

    pub_sub::ZenohTypedSubscriber<CanFrame> can_subscriber(
        can_key,
        [&parser, &frames_in, &decoded](CanFrame::Reader message)
        {
            frames_in.touch();
            // The real length, not a padded buffer: a frame shorter than the
            // message it claims to be must be rejected, not decoded as though
            // the padding were readings.
            const helpers::CanFrame frame = pub_sub::fromCapnp(message);
            if (parser.handle_can_frame(frame.id, frame.data_span()))
            {
                decoded.touch();
            }
        });

    health.markReady();

    while (!cli::interrupted())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        health.kick();
    }

    SPDLOG_INFO("Interrupted; shutting down.");
    return 0;
}


