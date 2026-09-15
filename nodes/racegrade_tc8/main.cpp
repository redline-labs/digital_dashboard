#include "cli/interrupt.h"
#include "node_health/reporter.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/zenoh_service.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "racegrade_tc8_configure.capnp.h"
#include "racegrade_tc8_signals.capnp.h"
#include "dbc_motec_e888_rev1_parser.h"
#include "racegrade_tc8_messages.h"
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
    racegrade_tc8::fillInputs(msg, inputs_pub.fields());
    inputs_pub.put();
}

static void handle_diagnostics_message(const dbc_motec_e888_rev1::Diagnostics_t& msg, pub_sub::ZenohPublisher<RaceGradeTc8Diagnostics>& diagnostics_pub)
{
    racegrade_tc8::fillDiagnostics(msg, diagnostics_pub.fields());
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
        ("s,source", "Zenoh key carrying CAN frames",
            cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("p,prefix", "Zenoh key prefix for this node's topics",
            cxxopts::value<std::string>()->default_value("nodes/racegrade_tc8"))
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

    const std::string can_key = args["source"].as<std::string>();
    const std::string prefix = args["prefix"].as<std::string>();

    // Create the publishers for the Inputs and Diagnostics messages
    pub_sub::ZenohPublisher<RaceGradeTc8Inputs> inputs_pub(prefix + "/inputs");
    pub_sub::ZenohPublisher<RaceGradeTc8Diagnostics> diagnostics_pub(prefix + "/diagnostics");

    dbc_motec_e888_rev1::dbc_motec_e888_rev1_parser parser;
    parser.on_Inputs([&inputs_pub](const dbc_motec_e888_rev1::Inputs_t& msg){
        handle_input_message(msg, inputs_pub);
    });
    parser.on_Diagnostics([&diagnostics_pub](const dbc_motec_e888_rev1::Diagnostics_t& msg){
        handle_diagnostics_message(msg, diagnostics_pub);
    });

    // `configure`, not the `hello` this was scaffolded with: the key is the
    // service's name to everything that discovers it.
    const std::string keyexpr = prefix + "/configure";
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


