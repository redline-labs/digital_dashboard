#include "dbc_motec_ltc_rev1_parser.h"
#include "motec_ltc_messages.h"

#include "cli/interrupt.h"
#include "node_health/can_decoder.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "motec_ltc.capnp.h"

#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>

static void publish_ltc(const dbc_motec_ltc_rev1::LTC_1_ID1_t& m, pub_sub::ZenohPublisher<MotecLtcTelemetry>& pub)
{
    motec_ltc::fillTelemetry(m, pub.fields());
    pub.put();
}

int main(int argc, char** argv)
{
    cxxopts::Options options("motec_ltc", "MoTeC LTC node");
    options.add_options()
        ("s,source", "Zenoh key carrying CAN frames",
            cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("p,prefix", "Zenoh key prefix for this node's topics",
            cxxopts::value<std::string>()->default_value("nodes/motec_ltc"))
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
    core::setupLogging({.program = "motec_ltc", .debug = result["debug"].as<bool>()});

    const std::string can_key = result["source"].as<std::string>();
    const std::string prefix = result["prefix"].as<std::string>();

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("motec_ltc");

    // Early, so a SIGTERM during startup still ends in a clean exit.
    cli::installInterruptHandler();

    // `nodes/<node>/<stream>`, like every other node. This used to publish on
    // `vehicle/lambda0`, which read as a vehicle-wide signal rather than as one
    // node's output; --prefix restores the old key for a config that wants it.
    pub_sub::ZenohPublisher<MotecLtcTelemetry> ltc_pub(prefix + "/telemetry");

    dbc_motec_ltc_rev1::dbc_motec_ltc_rev1_parser parser;
    parser.on_LTC_1_ID1([&](const dbc_motec_ltc_rev1::LTC_1_ID1_t& msg){
        publish_ltc(msg, ltc_pub);
    });

    // Health, the CAN subscription and the wait for SIGTERM: the same in
    // every decoder node.
    node_health::runCanDecoder("motec_ltc", can_key, [&parser](const helpers::CanFrame& frame) {
        return parser.handle_can_frame(frame.id, frame.data_span());
    });
    return 0;
}


