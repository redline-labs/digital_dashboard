#include "dbc_megasquirt_dash_data_parser.h"

#include "cli/interrupt.h"
#include "node_health/reporter.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"
#include "megasquirt.capnp.h"

#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>
#include <tuple>
#include <type_traits>
#include <algorithm>
#include <thread>
#include <chrono>

using namespace dbc_megasquirt_dash_data;

static void publish_dash(const dbc_megasquirt_dash_data_parser::db_t& db, pub_sub::ZenohPublisher<MegasquirtDash>& pub)
{
    auto& out = pub.fields();
    const auto& dash0 = db.megasquirt_dash0;
    const auto& dash1 = db.megasquirt_dash1;
    const auto& dash2 = db.megasquirt_dash2;
    const auto& dash3 = db.megasquirt_dash3;
    const auto& dash4 = db.megasquirt_dash4;

    // Frame 0
    out.setRpm(dash0.rpm);
    out.setMapKpa(dash0.map);
    out.setTpsPct(dash0.tps);
    out.setCoolantTempF(dash0.clt);

    // Frame 1
    out.setIgnitionAdvanceDeg(dash1.adv_deg);
    out.setIntakeAirTempF(dash1.mat);
    out.setInjPw1Ms(dash1.pw1);
    out.setInjPw2Ms(dash1.pw2);

    // Frame 2
    out.setSeqPw1Ms(dash2.pwseq1);
    out.setEgt1F(dash2.egt1);
    out.setEgoCorrectionPct(dash2.egocor1);
    out.setAfr1(dash2.AFR1);
    out.setAfrTarget1(dash2.afrtgt1);

    // Frame 3
    out.setKnockRetardDeg(dash3.knk_rtd);
    out.setSensor1(dash3.sensors1);
    out.setSensor2(dash3.sensors2);
    out.setBatteryVolts(dash3.batt);

    // Frame 4
    out.setLaunchTimingDeg(dash4.launch_timing);
    out.setTcRetard(dash4.tc_retard);
    out.setVssMps(dash4.VSS1);

    pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    core::setupLogging({.program = "megasquirt"});

    cxxopts::Options options("megasquirt", "Megasquirt dash node");
    options.add_options()
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("megasquirt");

    // SIGINT and SIGTERM both set the flag the loop below polls.
    cli::installInterruptHandler();

    pub_sub::ZenohPublisher<MegasquirtDash> dash_pub("nodes/megasquirt/dash");

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

    // Declared before the subscriber, so the subscriber is destroyed first and
    // no callback can touch a check that has gone away.
    node_health::HealthReporter health("megasquirt");
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


