#include "carplay/config.h"
#include "carplay/message.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>


const std::array kGSSendDPI = std::to_array<uint8_t>({
    170, 85, 170, 85, 28, 0, 0, 0, 153, 0, 0, 0, 102, 255, 255, 255, 16, 0, 0, 0, 47, 116, 109, 112, 47, 115, 99, 114,
    101, 101, 110, 95, 100, 112, 105, 0, 4, 0, 0, 0, 160, 0, 0, 0
});

const std::array kGSSendOpen = std::to_array<uint8_t>({
    170, 85, 170, 85, 28, 0, 0, 0, 1, 0, 0, 0, 254, 255, 255, 255, 177, 2, 0, 0, 219, 2, 0, 0, 60, 0, 0, 0, 5, 0, 0, 0,
    0, 192, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0
});

const std::array kGSSendNightMode = std::to_array<uint8_t>({
    170, 85, 170, 85, 28, 0, 0, 0, 153, 0, 0, 0, 102, 255, 255, 255, 16, 0, 0, 0, 47, 116, 109, 112, 47, 110, 105, 103,
    104, 116, 95, 109, 111, 100, 101, 0, 4, 0, 0, 0, 0, 0, 0, 0
});

const std::array kGSSendDriveSide = std::to_array<uint8_t>({
    170, 85, 170, 85, 33, 0, 0, 0, 153, 0, 0, 0, 102, 255, 255, 255, 21, 0, 0, 0, 47, 116, 109, 112, 47, 104, 97, 110,
    100, 95, 100, 114, 105, 118, 101, 95, 109, 111, 100, 101, 0, 4, 0, 0, 0, 0, 0, 0, 0
});

const std::array kGSSendChargeMode = std::to_array<uint8_t>({
    170, 85, 170, 85, 29, 0, 0, 0, 153, 0, 0, 0, 102, 255, 255, 255, 17, 0, 0, 0, 47, 116, 109, 112, 47, 99, 104, 97,
    114, 103, 101, 95, 109, 111, 100, 101, 0, 4, 0, 0, 0, 1, 0, 0, 0
});

const std::array kGSSendBoxName = std::to_array<uint8_t>({
    170, 85, 170, 85, 30, 0, 0, 0, 153, 0, 0, 0, 102, 255, 255, 255, 14, 0, 0, 0, 47, 101, 116, 99, 47, 98, 111, 120,
    95, 110, 97, 109, 101, 0, 8, 0, 0, 0, 110, 111, 100, 101, 80, 108, 97, 121
});

const std::array kGSSendBoxCfg = std::to_array<uint8_t>({
    170, 85, 170, 85, 86, 0, 0, 0, 25, 0, 0, 0, 230, 255, 255, 255, 123, 34, 109, 101, 100, 105, 97, 68, 101, 108, 97,
    121, 34, 58, 51, 48, 48, 44, 34, 115, 121, 110, 99, 84, 105, 109, 101, 34, 58, 49, 55, 50, 50, 55, 52, 51, 54, 51,
    50, 44, 34, 97, 110, 100, 114, 111, 105, 100, 65, 117, 116, 111, 83, 105, 122, 101, 87, 34, 58, 54, 56, 57, 44, 34,
    97, 110, 100, 114, 111, 105, 100, 65, 117, 116, 111, 83, 105, 122, 101, 72, 34, 58, 55, 51, 49, 125
});

const std::array kGSSendWifiEnable = std::to_array<uint8_t>({
    170, 85, 170, 85, 4, 0, 0, 0, 8, 0, 0, 0, 247, 255, 255, 255, 232, 3, 0, 0
});

const std::array kGSSendWifiBand = std::to_array<uint8_t>({
    170, 85, 170, 85, 4, 0, 0, 0, 8, 0, 0, 0, 247, 255, 255, 255, 25, 0, 0, 0
});

const std::array kGSSendMicMode = std::to_array<uint8_t>({
    170, 85, 170, 85, 4, 0, 0, 0, 8, 0, 0, 0, 247, 255, 255, 255, 7, 0, 0, 0
});

const std::array kGSSendAudioTransfer = std::to_array<uint8_t>({
    170, 85, 170, 85, 4, 0, 0, 0, 8, 0, 0, 0, 247, 255, 255, 255, 23, 0, 0, 0
});

template<size_t N>
static bool test(std::string_view test_name, const std::vector<uint8_t>& generated, const std::array<uint8_t, N>& golden)
{
    bool matched = (generated.size() == golden.size());
    size_t fail_idx = 0;

    for (fail_idx = 0; fail_idx < std::min(generated.size(), golden.size()); ++fail_idx)
    {
        if (generated[fail_idx] != golden[fail_idx])
        {
            matched = false;
            break;
        }
    }

    if (matched == true)
    {
        SPDLOG_INFO("Test {} passed.", test_name);
    }
    else
    {
        SPDLOG_ERROR("Test {} failed at index {}.", test_name, fail_idx);
        SPDLOG_ERROR("Generated: {}", spdlog::to_hex(generated));
        SPDLOG_ERROR("Golden:    {}", spdlog::to_hex(golden));
    }

    return matched;
}


int main(int /*argc*/, char** /*argv*/)
{
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    std::vector<uint8_t> usb_request;
    
    constexpr uint32_t width_px = 689u;
    constexpr uint32_t height_px = 731u;

    CarplayConfig_t cfg;
    cfg.dpi = 160;
    cfg.fps = 60;
    cfg.format = 5;
    cfg.packet_max = 49152;
    cfg.i_box_version = 2;
    cfg.phone_work_mode = 2;
    cfg.media_delay = 300;
    cfg.box_name = "nodePlay";

    usb_request = SendNumber(DongleConfigFile::DPI, cfg.dpi).serialize();
    test("SendDPI", usb_request, kGSSendDPI);

    usb_request = SendOpen(cfg, width_px, height_px).serialize();
    test("SendOpen", usb_request, kGSSendOpen);

    usb_request = SendBoolean(DongleConfigFile::NightMode, cfg.night_mode).serialize();
    test("SendNightMode", usb_request, kGSSendNightMode);

    usb_request = SendNumber(DongleConfigFile::HandDriveMode, static_cast<uint32_t>(cfg.drive_type)).serialize();
    test("SendDriveSide", usb_request, kGSSendDriveSide);

    usb_request = SendBoolean(DongleConfigFile::ChargeMode, true).serialize();
    test("SendChargeMode", usb_request, kGSSendChargeMode);

    usb_request = SendString(DongleConfigFile::BoxName, cfg.box_name).serialize();
    test("SendBoxName", usb_request, kGSSendBoxName);

    usb_request = SendBoxSettings(cfg, 1722743632, width_px, height_px).serialize();
    test("SendBoxSettings", usb_request, kGSSendBoxCfg);

    usb_request = Command(CommandMapping::WifiEnable).serialize();
    test("SendWiFiEnable", usb_request, kGSSendWifiEnable);

    usb_request = Command(CommandMapping::Wifi5g).serialize();
    test("SendWifiBand", usb_request, kGSSendWifiBand);

    usb_request = Command(CommandMapping::Mic).serialize();
    test("SendMicMode", usb_request, kGSSendMicMode);

    usb_request = Command(CommandMapping::AudioTransferOff).serialize();
    test("SendAudioTransfer", usb_request, kGSSendAudioTransfer);
}
