#include "dbc_megasquirt_dash_data_parser.h"
#include "megasquirt_messages.h"

#include "cli/interrupt.h"
#include "node_health/can_decoder.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "megasquirt.capnp.h"

#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>
#include <tuple>
#include <type_traits>
#include <algorithm>

using namespace dbc_megasquirt_dash_data;

static void publish_dash(const dbc_megasquirt_dash_data::dbc_megasquirt_dash_data_parser::db_t& db, pub_sub::ZenohPublisher<MegasquirtDash>& pub)
{
    megasquirt::fillDash(db, pub.fields());
    pub.put();
}

int main(int argc, char** argv)
{
    cxxopts::Options options("megasquirt", "Megasquirt dash node");
    options.add_options()
        ("s,source", "Zenoh key carrying CAN frames",
            cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("p,prefix", "Zenoh key prefix for this node's topics",
            cxxopts::value<std::string>()->default_value("nodes/megasquirt"))
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
    core::setupLogging({.program = "megasquirt", .debug = result["debug"].as<bool>()});

    const std::string can_key = result["source"].as<std::string>();
    const std::string prefix = result["prefix"].as<std::string>();

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("megasquirt");

    // Early, so a SIGTERM during startup still ends in a clean exit.
    cli::installInterruptHandler();

    pub_sub::ZenohPublisher<MegasquirtDash> dash_pub(prefix + "/dash");

    dbc_megasquirt_dash_data_parser parser;

    // Lump the five messages into a single callback.
    using Messages = dbc_megasquirt_dash_data::dbc_megasquirt_dash_data_t::Messages;
    parser.add_message_aggregator<
        Messages::megasquirt_dash0,
        Messages::megasquirt_dash1,
        Messages::megasquirt_dash2,
        Messages::megasquirt_dash3,
        Messages::megasquirt_dash4
    >([&dash_pub](const dbc_megasquirt_dash_data::dbc_megasquirt_dash_data_t& db){
        publish_dash(db, dash_pub);
    });

    // Health, the CAN subscription and the wait for SIGTERM: the same in
    // every decoder node.
    node_health::runCanDecoder("megasquirt", can_key, [&parser](const helpers::CanFrame& frame) {
        return parser.handle_can_frame(frame.id, frame.data_span());
    });
    return 0;
}


