#include "cli/interrupt.h"
#include "node_health/can_decoder.h"
#include "pub_sub/zenoh_service.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "racegrade_tc8_configure.capnp.h"
#include "racegrade_tc8_signals.capnp.h"
#include "dbc_motec_e888_rev1_parser.h"
#include "racegrade_tc8_messages.h"

#include <span>
#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include "core/core.h"
#include <zenoh.hxx>

#include <array>

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

    // Early, so a SIGTERM during startup still ends in a clean exit.
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

    // Health, the CAN subscription and the wait for SIGTERM: the same in
    // every decoder node.
    node_health::runCanDecoder("racegrade_tc8", can_key, [&parser](const helpers::CanFrame& frame) {
        return parser.handle_can_frame(frame.id, frame.data_span());
    });
    return 0;
}


