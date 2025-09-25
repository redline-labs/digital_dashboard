#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <chrono>
#include <thread>
#include <vector>
#include <array>

#include "pub_sub/zenoh_publisher.h"
#include "can_frame.capnp.h"

#include "dbc_motec_e888_rev1.h"

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("mock_racegrade_tc8_frames", "Publishes simulated Racegrade TC8 CAN frames");
    options.add_options()
        ("k,key", "Zenoh key to publish frames to", cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string key = result["key"].as<std::string>();
    SPDLOG_INFO("Publishing simulated TC8 CAN frames to '{}'", key);

    pub_sub::ZenohPublisher<CanFrame> pub(key);

    // Initialize fixed-size payload buffer ONCE and reuse it to avoid message growth.
    pub.fields().setLen(8u);
    if (!pub.fields().hasData() || pub.fields().getData().size() != 8u)
    {
        (void)pub.fields().initData(8u);
    }
    pub.fields().setId(dbc_motec_e888_rev1::Inputs_t::id);

    dbc_motec_e888_rev1::Inputs_t msg = {};

    float iteration = 0.0f;
    while (true)
    {
        msg.AV1 = std::sin(iteration) * 400.0f + 600.0f;
        msg.AV2 = std::sin(iteration) * 400.0f + 500.0f;
        msg.AV3 = std::sin(iteration) * 400.0f + 400.0f;
        msg.AV4 = std::sin(iteration) * 400.0f + 300.0f;
        msg.AV5 = std::sin(iteration) * 400.0f + 300.0f;
        msg.AV6 = std::sin(iteration) * 400.0f + 200.0f;
        msg.AV7 = std::sin(iteration) * 400.0f + 100.0f;
        msg.AV8 = std::sin(iteration) * 400.0f;
        msg.TC1 = std::sin(iteration) * 400.0f + 100.0f;
        msg.TC2 = std::sin(iteration) * 400.0f + 200.0f;
        msg.TC3 = std::sin(iteration) * 400.0f + 300.0f;
        msg.TC4 = std::sin(iteration) * 400.0f + 400.0f;
        msg.TC5 = std::sin(iteration) * 400.0f + 500.0f;
        msg.TC6 = std::sin(iteration) * 400.0f + 600.0f;
        msg.TC7 = std::sin(iteration) * 400.0f + 500.0f;
        msg.TC8 = std::sin(iteration) * 400.0f + 400.0f;
        msg.Freq1 = std::sin(iteration) * 400.0f + 300.0f;
        msg.Freq2 = std::sin(iteration) * 400.0f + 200.0f;
        msg.Freq3 = std::sin(iteration) * 400.0f + 100.0f;
        msg.Freq4 = std::sin(iteration) * 400.0f;

        iteration += 0.01f;

        for (const auto& multiplexor_index : dbc_motec_e888_rev1::Inputs_t::multiplexor_group_indexes)
        {
            msg.mux() = multiplexor_index;

            const std::array<uint8_t, 8u> payload = msg.encode();
            
            auto data = pub.fields().getData();
            for (size_t i = 0u; i < 8u; ++i)
            {
                data.set(i, payload[i]);
            }

            pub.put();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return 0;
}


