#include "dbc_motec_ltc_rev1_parser.h"

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
    using SensorState = dbc_motec_ltc_rev1::LTC_1_ID1_t::sig_LTC1_SensorState_t::Values;

    auto& out = pub.fields();
    out.setIndex(m.LTC1_Index);
    out.setLambda(m.LTC1_Lambda);
    out.setIpn(m.LTC1_Ipn);
    out.setInternalTempC(m.LTC1_InternalTemp);

    out.setSensorControlFault(static_cast<bool>(m.LTC1_SensorControlFault));
    out.setInternalFault(static_cast<bool>(m.LTC1_InternalFault));
    out.setSensorWireShort(static_cast<bool>(m.LTC1_SensorWireShort));
    out.setHeaterFailedToHeat(static_cast<bool>(m.LTC1_HeaterFailedtoHeat));
    out.setHeaterOpenCircuit(static_cast<bool>(m.LTC1_HeaterOpenCircuit));
    out.setHeaterShortToVbatt(static_cast<bool>(m.LTC1_HeaterShorttoVBATT));
    out.setHeaterShortToGnd(static_cast<bool>(m.LTC1_HeaterShorttoGND));

    out.setHeaterDutyCyclePct(m.LTC1_HeaterDutyCycle);

    switch (m.LTC1_SensorState)
    {
        case SensorState::START:
            out.setSensorState(LtcSensorState::START);
            break;

        case SensorState::DIAGNOSTICS:
            out.setSensorState(LtcSensorState::DIAGNOSTICS);
            break;

        case SensorState::PRE_CAL:
            out.setSensorState(LtcSensorState::PRE_CAL);
            break;

        case SensorState::CALIBRATION:
            out.setSensorState(LtcSensorState::CALIBRATION);
            break;

        case SensorState::POST_CAL:
            out.setSensorState(LtcSensorState::POST_CAL);
            break;

        case SensorState::PAUSED:
            out.setSensorState(LtcSensorState::PAUSED);
            break;

        case SensorState::HEATING:
            out.setSensorState(LtcSensorState::HEATING);
            break;

        case SensorState::RUNNING:
            out.setSensorState(LtcSensorState::RUNNING);
            break;

        case SensorState::COOLING:
            out.setSensorState(LtcSensorState::COOLING);
            break;

        case SensorState::PUMP_START:
            out.setSensorState(LtcSensorState::PUMP_START);
            break;

        case SensorState::PUMP_OFF:
            out.setSensorState(LtcSensorState::PUMP_OFF);
            break;

        default:
            out.setSensorState(LtcSensorState::START);
            break;
    }

    out.setBattVolts(m.LTC1_BattVolts);
    out.setIp(m.LTC1_Ip);
    out.setRi(m.LTC1_Ri);

    pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    core::setupLogging({.program = "motec_ltc"});

    cxxopts::Options options("motec_ltc", "MoTeC LTC node");
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
    pub_sub::NodeIdentity node_identity("motec_ltc");

    // SIGINT and SIGTERM both set the flag the loop below polls.
    cli::installInterruptHandler();

    pub_sub::ZenohPublisher<MotecLtcTelemetry> ltc_pub("vehicle/lambda0");

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


