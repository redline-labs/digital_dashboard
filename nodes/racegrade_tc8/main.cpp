#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <zenoh.hxx>
#include <thread>
#include <chrono>
#include <pub_sub/zenoh_service.h>
#include "racegrade_tc8_configure.capnp.h"

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

    // Open a zenoh session with default config
    try
    {
        const char* keyexpr = "nodes/racegrade_tc8/hello";
        SPDLOG_INFO("Declaring queryable on '{}'", keyexpr);

        pub_sub::ZenohService<RaceGradeTc8ConfigureRequest, RaceGradeTc8ConfigureResponse> svc(
            keyexpr,
            [](const RaceGradeTc8ConfigureRequest::Reader& req, RaceGradeTc8ConfigureResponse::Builder& resp)
            {
                SPDLOG_INFO("Received request: messageFormat={}, transmitRate={}, canId={}",
                            static_cast<int>(req.getMessageFormat()),
                            static_cast<int>(req.getTransmitRate()),
                            req.getCanId());
                resp.setResponse(true);
            }
        );

        SPDLOG_INFO("hello world (zenoh queryable ready)");

        // Keep the process alive; Ctrl+C to exit
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to start zenoh queryable: {}", e.what());
        return 1;
    }
    return 0;
}


