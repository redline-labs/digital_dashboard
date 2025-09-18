#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <zenoh.hxx>
#include <zenoh/api/channels.hxx>
#include <string>
#include <chrono>
#include <variant>
#include <pub_sub/zenoh_client.h>
#include "racegrade_tc8_configure.capnp.h"

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("racegrade_tc8_querier", "Querier for racegrade_tc8 node");
    options.add_options()
        ("k,key", "Key expression to query", cxxopts::value<std::string>()->default_value("nodes/racegrade_tc8/hello"))
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string key = result["key"].as<std::string>();

    SPDLOG_INFO("Sending GET to '{}'", key);

    pub_sub::ZenohClient<RaceGradeTc8ConfigureRequest, RaceGradeTc8ConfigureResponse> client(key, 2000);

    // Use owned builder via fields()
    client.fields().setMessageFormat(RaceGradeTc8ConfigureRequest::MessageFormat::E888_ID0X0_F0);
    client.fields().setTransmitRate(RaceGradeTc8ConfigureRequest::TransmitRate::RATE10_HZ);
    client.fields().setCanId(0x0F0);

    bool got = client.request([](const RaceGradeTc8ConfigureResponse::Reader& resp){
        SPDLOG_INFO("Response: {}", resp.getResponse() ? "ok" : "fail");
    });
    if (!got)
    {
        SPDLOG_WARN("No response received");
    }

    return 0;
}


