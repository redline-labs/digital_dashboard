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
    // setData every frame, never getData and set(): ZenohPublisher::put()
    // reconstructs the message arena and re-inits the root, so any list
    // allocated for a previous frame is gone by now and getData() would hand
    // back a zero-length list. capnp's bounds check on set() is KJ_IREQUIRE,
    // which is compiled out in a release build, so writing into that list is a
    // straight out-of-bounds store rather than an assertion failure. setData
    // allocates the list and copies in one step.
    pub.fields().setData(kj::arrayPtr(bytes.data(), bytes.size()));
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

    using namespace dbc_megasquirt_dash_data;

    float t = 0.0f;
    const auto frame_period = 50ms;
    const auto inter_frame_delay = 5ms; // between messages

    while (true)
    {
        const auto loop_start = steady_clock::now();

        // dash0
        {
            megasquirt_dash0_t m{};
            m.tps = 50.0f + 30.0f * std::sin(t);
            m.clt = 190.0f + 10.0f * std::sin(t * 0.2f); // F
            m.rpm = static_cast<uint16_t>(1500.0f + 500.0f * std::sin(t * 0.5f));
            m.map = 100.0f + 20.0f * std::sin(t * 0.3f);  // kPa
            set_payload(pub, megasquirt_dash0_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash1
        {
            megasquirt_dash1_t m{};
            m.adv_deg = 10.0f + 5.0f * std::sin(t * 0.4f);
            m.mat = 100.0f + 10.0f * std::sin(t * 0.25f); // F
            m.pw2 = 3.0f + 0.5f * std::sin(t * 0.6f);
            m.pw1 = 3.0f + 0.5f * std::cos(t * 0.6f);
            set_payload(pub, megasquirt_dash1_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash2
        {
            megasquirt_dash2_t m{};
            m.pwseq1 = 2.0f + 0.2f * std::sin(t * 0.8f);
            m.egt1 = 1200.0f + 50.0f * std::sin(t * 0.15f); // F
            m.egocor1 = 100.0f + 2.0f * std::sin(t * 0.7f); // %
            // Physical units. These were raw-looking 140 and 145, which a 0.1 scale
            // saturated to 25.5.
            m.AFR1 = 14.0f + 0.2f * std::sin(t * 0.33f);
            m.afrtgt1 = 14.5f;
            set_payload(pub, megasquirt_dash2_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash3
        {
            megasquirt_dash3_t m{};
            m.knk_rtd = 0.0f;
            m.sensors2 = 1.23f + 0.1f * std::sin(t);
            m.sensors1 = 2.34f + 0.1f * std::cos(t);
            m.batt = 13.8f + 0.2f * std::sin(t * 0.5f);
            set_payload(pub, megasquirt_dash3_t::id, m.encode());
            pub.put();
        }
        std::this_thread::sleep_for(inter_frame_delay);

        // dash4
        {
            megasquirt_dash4_t m{};
            m.launch_timing = 0.0f;
            m.tc_retard = 0.0f;
            m.VSS1 = 10.0f + 2.0f * std::sin(t * 0.9f); // m/s
            set_payload(pub, megasquirt_dash4_t::id, m.encode());
            pub.put();
        }

        // Sleep the remaining time to maintain ~50ms loop period
        const auto elapsed = steady_clock::now() - loop_start;
        if (elapsed < frame_period)
        {
            std::this_thread::sleep_for(frame_period - elapsed);
        }

        t += 0.05f;
    }

    return 0;
}


