#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <chrono>
#include <thread>
#include <array>
#include <cmath>

#include "pub_sub/zenoh_publisher.h"
#include "can_frame.capnp.h"

#include "dbc_megasquirt_dash_data.h"

using namespace std::chrono;

static void set_payload(pub_sub::ZenohPublisher<CanFrame>& pub,
                        uint32_t id,
                        const std::array<uint8_t, 8u>& bytes)
{
    pub.fields().setId(id);
    pub.fields().setLen(8u);
    auto data = pub.fields().getData();
    for (size_t i = 0; i < 8u; ++i)
    {
        data.set(i, bytes[i]);
    }
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("megasquirt_test_frames", "Publishes simulated Megasquirt dash CAN frames (dash0..4)");
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
    SPDLOG_INFO("Publishing simulated Megasquirt CAN frames to '{}'", key);

    pub_sub::ZenohPublisher<CanFrame> pub(key);
    // Initialize fixed-size payload buffer once and reuse it
    pub.fields().setLen(8u);
    if (!pub.fields().hasData() || pub.fields().getData().size() != 8u)
    {
        (void)pub.fields().initData(8u);
    }

    using namespace dbc_megasquirt_dash_data;

    double t = 0.0;
    const auto frame_period = 50ms;
    const auto inter_frame_delay = 5ms; // between messages

    while (true)
    {
        const auto loop_start = steady_clock::now();

        // dash0
        {
            megasquirt_dash0_t m{};
            m.tps = static_cast<double>(50.0 + 30.0 * std::sin(t));
            m.clt = static_cast<double>(190.0 + 10.0 * std::sin(t * 0.2)); // F
            m.rpm = static_cast<uint64_t>(1500 + 500 * std::sin(t * 0.5));
            m.map = static_cast<double>(100.0 + 20.0 * std::sin(t * 0.3));  // kPa
            set_payload(pub, megasquirt_dash0_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash1
        {
            megasquirt_dash1_t m{};
            m.adv_deg = static_cast<double>(10.0 + 5.0 * std::sin(t * 0.4));
            m.mat = static_cast<double>(100.0 + 10.0 * std::sin(t * 0.25)); // F
            m.pw2 = static_cast<double>(3.0 + 0.5 * std::sin(t * 0.6));
            m.pw1 = static_cast<double>(3.0 + 0.5 * std::cos(t * 0.6));
            set_payload(pub, megasquirt_dash1_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash2
        {
            megasquirt_dash2_t m{};
            m.pwseq1 = static_cast<double>(2.0 + 0.2 * std::sin(t * 0.8));
            m.egt1 = static_cast<double>(1200.0 + 50.0 * std::sin(t * 0.15)); // F
            m.egocor1 = static_cast<double>(100.0 + 2.0 * std::sin(t * 0.7)); // %
            m.AFR1 = static_cast<uint64_t>(static_cast<uint64_t>(140 + 2 * std::sin(t * 0.33))); // 14.0 ..
            m.afrtgt1 = static_cast<uint64_t>(145); // 14.5 target
            set_payload(pub, megasquirt_dash2_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash3
        {
            megasquirt_dash3_t m{};
            m.knk_rtd = static_cast<double>(0.0);
            m.sensors2 = static_cast<double>(1.23 + 0.1 * std::sin(t));
            m.sensors1 = static_cast<double>(2.34 + 0.1 * std::cos(t));
            m.batt = static_cast<double>(13.8 + 0.2 * std::sin(t * 0.5));
            set_payload(pub, megasquirt_dash3_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash4
        {
            megasquirt_dash4_t m{};
            m.launch_timing = static_cast<double>(0.0);
            m.tc_retard = static_cast<double>(0.0);
            m.VSS1 = static_cast<double>(10.0 + 2.0 * std::sin(t * 0.9)); // m/s
            set_payload(pub, megasquirt_dash4_t::id, m.encode());
            pub.put();
        }

        // Sleep the remaining time to maintain ~50ms loop period
        const auto elapsed = steady_clock::now() - loop_start;
        if (elapsed < frame_period)
        {
            std::this_thread::sleep_for(frame_period - elapsed);
        }

        t += 0.05;
    }

    return 0;
}


