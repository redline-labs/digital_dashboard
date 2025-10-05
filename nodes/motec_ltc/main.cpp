#include "dbc_motec_ltc_rev1_parser.h"

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"
#include "motec_ltc.capnp.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>
#include <chrono>
#include <thread>

static void publish_ltc(const dbc_motec_ltc_rev1::LTC_1_ID1_t& m, pub_sub::ZenohPublisher<MotecLtcTelemetry>& pub)
{
    using SensorState = dbc_motec_ltc_rev1::LTC_1_ID1_t::sig_LTC1_SensorState_t::Values;

    auto& out = pub.fields();
    out.setIndex(static_cast<uint8_t>(m.LTC1_Index));
    out.setLambda(static_cast<float>(m.LTC1_Lambda));
    out.setIpn(static_cast<float>(m.LTC1_Ipn));
    out.setInternalTempC(static_cast<float>(m.LTC1_InternalTemp));

    out.setSensorControlFault(static_cast<bool>(m.LTC1_SensorControlFault));
    out.setInternalFault(static_cast<bool>(m.LTC1_InternalFault));
    out.setSensorWireShort(static_cast<bool>(m.LTC1_SensorWireShort));
    out.setHeaterFailedToHeat(static_cast<bool>(m.LTC1_HeaterFailedtoHeat));
    out.setHeaterOpenCircuit(static_cast<bool>(m.LTC1_HeaterOpenCircuit));
    out.setHeaterShortToVbatt(static_cast<bool>(m.LTC1_HeaterShorttoVBATT));
    out.setHeaterShortToGnd(static_cast<bool>(m.LTC1_HeaterShorttoGND));

    out.setHeaterDutyCyclePct(static_cast<float>(m.LTC1_HeaterDutyCycle));

    switch (m.LTC1_SensorState)
    {
        case SensorState::START_0:
            out.setSensorState(LtcSensorState::START);
            break;

        case SensorState::DIAGNOSTICS_1:
            out.setSensorState(LtcSensorState::DIAGNOSTICS);
            break;

        case SensorState::PRE_CAL_2:
            out.setSensorState(LtcSensorState::PRE_CAL);
            break;

        case SensorState::CALIBRATION_3:
            out.setSensorState(LtcSensorState::CALIBRATION);
            break;

        case SensorState::POST_CAL_4:
            out.setSensorState(LtcSensorState::POST_CAL);
            break;

        case SensorState::PAUSED_5:
            out.setSensorState(LtcSensorState::PAUSED);
            break;

        case SensorState::HEATING_6:
            out.setSensorState(LtcSensorState::HEATING);
            break;

        case SensorState::RUNNING_7:
            out.setSensorState(LtcSensorState::RUNNING);
            break;

        case SensorState::COOLING_8:
            out.setSensorState(LtcSensorState::COOLING);
            break;

        case SensorState::PUMP_START_9:
            out.setSensorState(LtcSensorState::PUMP_START);
            break;

        case SensorState::PUMP_OFF_10:
            out.setSensorState(LtcSensorState::PUMP_OFF);
            break;

        default:
            out.setSensorState(LtcSensorState::START);
            break;
    }

    out.setBattVolts(static_cast<float>(m.LTC1_BattVolts));
    out.setIp(static_cast<float>(m.LTC1_Ip));
    out.setRi(static_cast<float>(m.LTC1_Ri));

    pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("motec_ltc", "MoTeC LTC node");
    options.add_options()
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    pub_sub::ZenohPublisher<MotecLtcTelemetry> ltc_pub("vehicle/lambda0");

    dbc_motec_ltc_rev1::dbc_motec_ltc_rev1_parser parser;
    parser.on_LTC_1_ID1([&](const dbc_motec_ltc_rev1::LTC_1_ID1_t& msg){
        publish_ltc(msg, ltc_pub);
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


