#include "cli/interrupt.h"
#include "node_health/reporter.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/zenoh_service.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "racegrade_tc8_configure.capnp.h"
#include "racegrade_tc8_signals.capnp.h"
#include "dbc_motec_e888_rev1_parser.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"

#include <span>
#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include "core/core.h"
#include <zenoh.hxx>

#include <array>
#include <thread>
#include <chrono>

static void handle_service_request(const RaceGradeTc8ConfigureRequest::Reader& req, RaceGradeTc8ConfigureResponse::Builder& resp)
{
    SPDLOG_INFO("Received request: messageFormat={}, transmitRate={}, canId={}",
                static_cast<int>(req.getMessageFormat()),
                static_cast<int>(req.getTransmitRate()),
                req.getCanId());

    resp.setResponse(true);
}

static void handle_input_message(const dbc_motec_e888_rev1::Inputs_t& msg, pub_sub::ZenohPublisher<RaceGradeTc8Inputs>& inputs_pub)
{
    auto& outputs = inputs_pub.fields();
    outputs.setVoltage1(msg.AV1);
    outputs.setVoltage2(msg.AV2);
    outputs.setVoltage3(msg.AV3);
    outputs.setVoltage4(msg.AV4);
    outputs.setVoltage5(msg.AV5);
    outputs.setVoltage6(msg.AV6);
    outputs.setVoltage7(msg.AV7);
    outputs.setVoltage8(msg.AV8);
    outputs.setTemperature1(msg.TC1);
    outputs.setTemperature2(msg.TC2);
    outputs.setTemperature3(msg.TC3);
    outputs.setTemperature4(msg.TC4);
    outputs.setTemperature5(msg.TC5);
    outputs.setTemperature6(msg.TC6);
    outputs.setTemperature7(msg.TC7);
    outputs.setTemperature8(msg.TC8);
    outputs.setFrequency1(msg.Freq1);
    outputs.setFrequency2(msg.Freq2);
    outputs.setFrequency3(msg.Freq3);
    outputs.setFrequency4(msg.Freq4);

    inputs_pub.put();
}

static void handle_diagnostics_message(const dbc_motec_e888_rev1::Diagnostics_t& msg, pub_sub::ZenohPublisher<RaceGradeTc8Diagnostics>& diagnostics_pub)
{
    auto& outputs = diagnostics_pub.fields();
    outputs.setColdJunctionComp1(msg.Cold_Junct_Comp1);
    // Whole degrees from a 16-bit field offset by -200, so int32_t in the
    // decoder. Every value in that range is exact in the schema's Float32.
    outputs.setColdJunctionComp2(static_cast<float>(msg.Cold_Junct_Comp2));
    outputs.setE888IntTemp(static_cast<float>(msg.E888_Int_Temp));
    outputs.setDig1InState(msg.Dig_1_In_State);
    outputs.setDig2InState(msg.Dig_2_In_State);
    outputs.setDig3InState(msg.Dig_3_In_State);
    outputs.setDig4InState(msg.Dig_4_In_State);
    outputs.setDig5InState(msg.Dig_5_In_State);
    outputs.setDig6InState(msg.Dig_6_In_State);
    outputs.setBatteryVolts(msg.Battery_Volts);
    outputs.setE888StatusFlags(msg.E888_Status_Flags);
    outputs.setFirmwareVersion(msg.Firmware_Version);

    diagnostics_pub.put();
}

int main(int argc, char** argv)
{
    // This used to be core::init_core(argc, argv), which parsed the command line
    // as well as setting up logging -- under a hardcoded
    // cxxopts::Options("dashboard", "Vehicle instrument cluster."), so
    // `racegrade_tc8 --help` printed the dashboard's usage. The parse belongs to
    // the program that owns the options.
    cxxopts::Options options("racegrade_tc8", "RaceGrade TC8 node");
    options.add_options()
        ("debug", "Enable debug logging.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("h,help", "Print usage");

    cxxopts::ParseResult args;
    try
    {
        args = options.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        core::setupLogging(false);
        SPDLOG_CRITICAL("{}", e.what());
        SPDLOG_INFO("{}", options.help());
        return 1;
    }

    core::setupLogging(args["debug"].as<bool>());

    if (args.count("help") != 0)
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("racegrade_tc8");

    // SIGINT and SIGTERM both set the flag the loop below polls.
    cli::installInterruptHandler();

    // Create the publishers for the Inputs and Diagnostics messages
    pub_sub::ZenohPublisher<RaceGradeTc8Inputs> inputs_pub("nodes/racegrade_tc8/inputs");
    pub_sub::ZenohPublisher<RaceGradeTc8Diagnostics> diagnostics_pub("nodes/racegrade_tc8/diagnostics");

    dbc_motec_e888_rev1::dbc_motec_e888_rev1_parser parser;
    parser.on_Inputs([&inputs_pub](const dbc_motec_e888_rev1::Inputs_t& msg){
        handle_input_message(msg, inputs_pub);
    });
    parser.on_Diagnostics([&diagnostics_pub](const dbc_motec_e888_rev1::Diagnostics_t& msg){
        handle_diagnostics_message(msg, diagnostics_pub);
    });

// Open a zenoh session with default config
    const char* keyexpr = "nodes/racegrade_tc8/hello";
    SPDLOG_INFO("Declaring queryable on '{}'", keyexpr);

    pub_sub::ZenohService<RaceGradeTc8ConfigureRequest, RaceGradeTc8ConfigureResponse> service(
        keyexpr, handle_service_request);

    // Subscribe to CAN frames and feed parser using typed subscriber
    // Declared before the subscriber, so the subscriber is destroyed first and
    // no callback can touch a check that has gone away.
    node_health::HealthReporter health("racegrade_tc8");
    // Frames arriving at all, and frames this node could decode: a quiet bus
    // and a bus carrying nothing but other devices look identical otherwise.
    auto& frames_in = health.addActivityCheck("can_rx", std::chrono::seconds(1));
    auto& decoded = health.addActivityCheck("decoded", std::chrono::seconds(2));

    pub_sub::ZenohTypedSubscriber<CanFrame> can_subscriber(
        "vehicle/can0/rx",
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


