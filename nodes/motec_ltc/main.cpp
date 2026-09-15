#include "dbc_motec_ltc_rev1_parser.h"
#include "motec_ltc_messages.h"

#include "cli/interrupt.h"
#include "node_health/reporter.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"
#include "motec_ltc.capnp.h"

#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>
#include <chrono>
#include <thread>

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

    // SIGINT and SIGTERM both set the flag the loop below polls.
    cli::installInterruptHandler();

    // `nodes/<node>/<stream>`, like every other node. This used to publish on
    // `vehicle/lambda0`, which read as a vehicle-wide signal rather than as one
    // node's output; --prefix restores the old key for a config that wants it.
    pub_sub::ZenohPublisher<MotecLtcTelemetry> ltc_pub(prefix + "/telemetry");

    dbc_motec_ltc_rev1::dbc_motec_ltc_rev1_parser parser;
    parser.on_LTC_1_ID1([&](const dbc_motec_ltc_rev1::LTC_1_ID1_t& msg){
        publish_ltc(msg, ltc_pub);
    });

    // Declared before the subscriber, so the subscriber is destroyed first and
    // no callback can touch a check that has gone away.
    node_health::HealthReporter health("motec_ltc");
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


