#include "pub_sub/zenoh_service.h"
#include "pub_sub/zenoh_publisher.h"
#include "racegrade_tc8_configure.capnp.h"
#include "racegrade_tc8_signals.capnp.h"
#include "dbc_motec_e888_rev1_parser.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
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
    outputs.setColdJunctionComp2(msg.Cold_Junct_Comp2);
    outputs.setE888IntTemp(msg.E888_Int_Temp);
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
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("racegrade_tc8", "Racegrade TC8 node");
    options.add_options()
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

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

    // Keep the process alive; Ctrl+C to exit
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}


