#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <algorithm>
#include <thread>

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "pub_sub/zenoh_service.h"
#include "can_frame.capnp.h"
#include "helpers/can_frame.h"
#include "canopen/transport.h"
#include "grayhill_keypad.capnp.h"

#include "canopen_grayhill_node.h"
#include "canopen_grayhill_helpers.h"

static void send_frame(pub_sub::ZenohPublisher<::CanFrame>& pub, const helpers::CanFrame& f)
{
    auto& fields = pub.fields();
    fields.setId(f.id);
    fields.setLen(f.len);

    const size_t n = std::min<size_t>(f.data.size(), static_cast<size_t>(f.len));
    auto dataList = fields.initData(n);

    for (size_t i = 0; i < n; ++i)
    {
        dataList.set(i, f.data[i]);
    }

    pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("grayhill_keypad", "Grayhill 3K CANopen keypad node");
    options.add_options()
        ("node-id", "CANopen node id (1..127)", cxxopts::value<int>()->default_value("10"))
        ("rx-key", "Zenoh RX key", cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("tx-key", "Zenoh TX key", cxxopts::value<std::string>()->default_value("vehicle/can0/tx"))
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const uint8_t node_id = static_cast<uint8_t>(result["node-id"].as<int>());
    const std::string rx_key = result["rx-key"].as<std::string>();
    const std::string tx_key = result["tx-key"].as<std::string>();

    SPDLOG_INFO("grayhill_keypad starting: node_id={}, rx='{}', tx='{}'", node_id, rx_key, tx_key);

    pub_sub::ZenohPublisher<::CanFrame> tx_pub(tx_key);

    canopen_grayhill::node device(node_id);

    device.on_buttons([](uint8_t b1_8, uint8_t b9_16, uint8_t b17_24){
        SPDLOG_INFO("Buttons: [1..8]=0x{:02X} [9..16]=0x{:02X} [17..24]=0x{:02X}", b1_8, b9_16, b17_24);
    });

    pub_sub::ZenohTypedSubscriber<::CanFrame> rx_subscriber(
        rx_key,
        [&device](::CanFrame::Reader frame)
        {
            helpers::CanFrame f{};
            f.id = frame.getId();
            f.len = frame.getLen();
            auto dataList = frame.getData();
            const size_t n = std::min<size_t>(f.data.size(), std::min<size_t>(static_cast<size_t>(f.len), dataList.size()));
            for (size_t i = 0; i < n; ++i)
            {
                f.data[i] = static_cast<uint8_t>(dataList[i]);
            }

            (void)device.handle_frame(f);
        });

    // Move to PRE-OPERATIONAL, set heartbeat, then OPERATIONAL
    {
        // NMT pre-operational (0x80)
        auto nmt_preop = canopen::make_nmt(canopen::NmtCommand::EnterPreOperational, node_id);
        send_frame(tx_pub, nmt_preop);
        SPDLOG_INFO("Sent NMT pre-operational to node {}", node_id);

        // Configure Producer Heartbeat Time (0x1017) on the device so it emits
        // its OWN heartbeat frames (COB-ID 0x700 + node) reflecting real NMT state.
        // Preferred approach when supported; survives our process restarts and stays
        // consistent with the keypad's true state transitions.
        // SDO expedited download command: 0x2B for 16-bit data (cs=0010, n=2)
        auto sdo_hb = canopen::make_sdo_download_u16(node_id, 0x1017, 0x00, 1000);
        send_frame(tx_pub, sdo_hb);
        SPDLOG_INFO("Requested heartbeat time via SDO (0x1017)=1000ms");

        // NMT operational (0x01)
        auto nmt_start = canopen::make_nmt(canopen::NmtCommand::Start, node_id);
        send_frame(tx_pub, nmt_start);
        SPDLOG_INFO("Sent NMT start to node {}", node_id);
    }

    // Services for brightness control
    pub_sub::ZenohService<GrayhillSetIndicatorBrightnessRequest, GrayhillSetIndicatorBrightnessResponse> svc_ind(
        "nodes/grayhill_keypad/set_indicator_brightness",
        [node_id, &tx_pub](const GrayhillSetIndicatorBrightnessRequest::Reader& req, GrayhillSetIndicatorBrightnessResponse::Builder& rsp)
        {
            uint16_t indicator_brightness = req.getValue();
            uint16_t backlight_brightness = 0u;  // TODO.
            auto f = canopen_grayhill::pack_rpdo2_brightness(indicator_brightness, backlight_brightness, node_id);
            send_frame(tx_pub, f);
            rsp.setOk(true);
        });

    pub_sub::ZenohService<GrayhillSetBacklightBrightnessRequest, GrayhillSetBacklightBrightnessResponse> svc_bk(
        "nodes/grayhill_keypad/set_backlight_brightness",
        [node_id, &tx_pub](const GrayhillSetBacklightBrightnessRequest::Reader& req, GrayhillSetBacklightBrightnessResponse::Builder& rsp)
        {
            uint16_t indicator_brightness = 0u;  // TODO.
            uint16_t backlight_brightness = req.getValue();
            auto f = canopen_grayhill::pack_rpdo2_brightness(indicator_brightness, backlight_brightness, node_id);
            send_frame(tx_pub, f);
            rsp.setOk(true);
        });

    // Keep process alive
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}


