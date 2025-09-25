#include "dbc_msel_master_relay_parser.h"

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>

static void publish_status(const dbc_msel_master_relay::Master_Relay_0x6E4_t& msg,
                           pub_sub::ZenohPublisher<MselMasterRelayStatus>& status_pub)
{
    auto& out = status_pub.fields();
    out.setStatus(static_cast<uint8_t>(msg.status));
    out.setOverTempWarn(static_cast<bool>(msg.over_temp_warn));
    out.setExternalKill(static_cast<bool>(msg.external_kill));
    out.setDriverKill(static_cast<bool>(msg.driver_kill));
    out.setOverTempKill(static_cast<bool>(msg.over_temp_kill));
    out.setHighVoltageWarn(static_cast<bool>(msg.high_voltage_warn));
    out.setLowVoltageWarn(static_cast<bool>(msg.low_voltage_warn));
    out.setOverCurrentWarn(static_cast<bool>(msg.over_current_warn));
    out.setCanKill(static_cast<bool>(msg.CAN_kill));
    out.setTemperatureInternal(static_cast<float>(msg.temperature_internal));
    out.setLoadCurrent(static_cast<float>(msg.load_current));
    out.setVoltageOut(static_cast<float>(msg.voltage_out));
    status_pub.put();
}

static void publish_info(const dbc_msel_master_relay::Master_Relay_0x6E5_t& msg,
                         pub_sub::ZenohPublisher<MselMasterRelayInfo>& info_pub)
{
    auto& out = info_pub.fields();
    out.setShutdownCause2(static_cast<uint8_t>(msg.shutdown_cause_2));
    out.setShutdownCause(static_cast<uint8_t>(msg.shutdown_cause));
    out.setTimeSinceShutdown(static_cast<float>(msg.time_since_shutdown));
    out.setConfigShutdownDelay(static_cast<float>(msg.config_shutdown_delay));
    out.setConfigCanKill(static_cast<uint8_t>(msg.config_CAN_kill));
    out.setConfigCanBaud(static_cast<uint8_t>(msg.config_CAN_baud));
    out.setConfigOutputDrive(static_cast<uint8_t>(msg.config_output_drive));
    out.setSerialNo(static_cast<uint32_t>(msg.serial_no));
    out.setVoltageIn(static_cast<float>(msg.voltage_in));
    info_pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("msel_master_relay", "MSEL Master Relay node");
    options.add_options()
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    pub_sub::ZenohPublisher<MselMasterRelayStatus> status_pub("nodes/msel_master_relay/status");
    pub_sub::ZenohPublisher<MselMasterRelayInfo> info_pub("nodes/msel_master_relay/info");

    dbc_msel_master_relay::dbc_msel_master_relay_parser parser;
    parser.on_Master_Relay_0x6E4([&](const dbc_msel_master_relay::Master_Relay_0x6E4_t& msg){
        publish_status(msg, status_pub);
    });
    parser.on_Master_Relay_0x6E5([&](const dbc_msel_master_relay::Master_Relay_0x6E5_t& msg){
        publish_info(msg, info_pub);
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


