#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <chrono>
#include <thread>
#include <string>

#include "pcan_trc_parser/pcan_trc_parser.h"
#include "pub_sub/zenoh_publisher.h"
#include "can_frame.capnp.h"

using namespace std::chrono;

static void set_payload(pub_sub::ZenohPublisher<CanFrame>& pub,
                        uint32_t id,
                        const std::vector<uint8_t>& bytes)
{
    pub.fields().setId(id);
    const uint8_t len = static_cast<uint8_t>(bytes.size());
    pub.fields().setLen(len);
    auto data = pub.fields().hasData() ? pub.fields().getData() : pub.fields().initData(len);
    if (data.size() != len) {
        data = pub.fields().initData(len);
    }
    for (size_t i = 0; i < len; ++i) {
        data.set(i, bytes[i]);
    }
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] %v");

    cxxopts::Options options("can_replay", "Replay PCAN .trc CAN frames over zenoh, preserving timing");
    options.add_options()
        ("f,file", "Input .trc filepath", cxxopts::value<std::string>())
        ("k,key", "Zenoh key to publish frames to", cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("l,loop", "Loop the input file", cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help") || !result.count("file"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string filepath = result["file"].as<std::string>();
    const std::string key = result["key"].as<std::string>();
    const bool do_loop = result["loop"].as<bool>();

    SPDLOG_INFO("Replaying '{}' to '{}' (loop: {})", filepath, key, do_loop);

    // Publisher for raw CAN frames
    pub_sub::ZenohPublisher<CanFrame> pub(key);
    if (!pub.isValid())
    {
        SPDLOG_ERROR("Failed to create zenoh publisher for '{}'", key);
        return 1;
    }

    while (true)
    {
        std::size_t num_published = 0;
        auto on_frame = [&](const helpers::CanFrame& f) {
            std::vector<uint8_t> bytes;
            bytes.reserve(f.len);
            for (uint8_t i = 0; i < f.len; ++i) {
                bytes.push_back(f.data[i]);
            }
            set_payload(pub, f.id, bytes);
            pub.put();
            ++num_published;
            return true;
        };

        const std::size_t delivered = pcan_trc_parser::parse_file(filepath, on_frame);
        SPDLOG_INFO("Published {} frames from '{}'", delivered, filepath);

        if (!do_loop) break;
    }

    return 0;
}


