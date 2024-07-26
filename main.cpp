#include <cxxopts.hpp>
#include <spdlog/spdlog.h>


#include "app_config.h"
#include "dongle_driver.h"
#include "messages/message.h"

// Patches:
// LibUSB core for debug messages.
// spdlog tweakme to lower the default log level.


int main(int argc, char** argv)
{
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("carplay_app", "Spin up a CarPlay instance.");
    options.add_options()
        ("debug", "Enable Debugging.",cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("libusb_debug", "Enable LibUSB Debugging.",cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
    cxxopts::ParseResult args_result;

    try
    {
        args_result = options.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_CRITICAL(e.what());
        return -1;
    }

    spdlog::set_level(args_result["debug"].as<bool>() ? spdlog::level::debug : spdlog::level::info);


    auto cfg = load_app_config("/Users/ryan/src/carplay_cpp/config.yaml");

    SPDLOG_DEBUG("Using libusb: {}", DongleDriver::libusb_version());

    DongleDriver driver(args_result["libusb_debug"].as<bool>());

    if (driver.find_dongle() == true)
    {
        SPDLOG_INFO("Found CarPlay dongle.");
    }
    else
    {
        SPDLOG_ERROR("Failed to find CarPlay dongle.");
    }

    auto cmd = SendOpen(cfg);

    auto ret = cmd.serialize();

    for (const auto& byte : ret)
    {
        printf("%d ", byte);
    }
}