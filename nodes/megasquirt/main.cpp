#include "dbc_megasquirt_dash_data_parser.h"

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"

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
    out.setRpm(static_cast<uint16_t>(dash0.rpm));
    out.setMapKpa(static_cast<float>(dash0.map));
    out.setTpsPct(static_cast<float>(dash0.tps));
    out.setCoolantTempF(static_cast<float>(dash0.clt));

    // Frame 1
    out.setIgnitionAdvanceDeg(static_cast<float>(dash1.adv_deg));
    out.setIntakeAirTempF(static_cast<float>(dash1.mat));
    out.setInjPw1Ms(static_cast<float>(dash1.pw1));
    out.setInjPw2Ms(static_cast<float>(dash1.pw2));

    // Frame 2
    out.setSeqPw1Ms(static_cast<float>(dash2.pwseq1));
    out.setEgt1F(static_cast<float>(dash2.egt1));
    out.setEgoCorrectionPct(static_cast<float>(dash2.egocor1));
    out.setAfr1(static_cast<float>(dash2.AFR1));
    out.setAfrTarget1(static_cast<float>(dash2.afrtgt1));

    // Frame 3
    out.setKnockRetardDeg(static_cast<float>(dash3.knk_rtd));
    out.setSensor1(static_cast<float>(dash3.sensors1));
    out.setSensor2(static_cast<float>(dash3.sensors2));
    out.setBatteryVolts(static_cast<float>(dash3.batt));

    // Frame 4
    out.setLaunchTimingDeg(static_cast<float>(dash4.launch_timing));
    out.setTcRetard(static_cast<float>(dash4.tc_retard));
    out.setVssMps(static_cast<float>(dash4.VSS1));

    pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("megasquirt", "Megasquirt dash node");
    options.add_options()
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

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

    pub_sub::ZenohTypedSubscriber<CanFrame> can_subscriber(
        "vehicle/can0/rx",
        [&parser](CanFrame::Reader frame)
        {
            uint32_t id = frame.getId();
            uint8_t len = frame.getLen();
            auto dataList = frame.getData();

            std::array<uint8_t, 8u> bytes{};
            const size_t n = std::min<size_t>(8u, std::min<size_t>(len, dataList.size()));
            for (size_t i = 0; i < n; ++i)
            {
                bytes[i] = static_cast<uint8_t>(dataList[i]);
            }
            parser.handle_can_frame(id, bytes);
        });

    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}


