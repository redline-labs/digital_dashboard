#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bundled/format.h>

#include "canopen/eds_parser.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdio.h>

int main(int argc, char** argv)
{
    cxxopts::Options options("canopen_code_gen", "Generate C++ helpers from CANopen EDS");
    options.add_options()
        ("name", "Base name for generated files", cxxopts::value<std::string>())
        ("input", "Path to .eds", cxxopts::value<std::string>())
        ("output", "Output directory", cxxopts::value<std::string>())
        ("silent", "Reduce logging", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("h,help", "Help");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        std::cout << options.help() << "\n";
        return 0;
    }

    bool silent = result["silent"].as<bool>();
    if (silent == true)
    {
        spdlog::set_level(spdlog::level::warn);
    }

    const std::string base = result["name"].as<std::string>();
    const std::string input = result["input"].as<std::string>();
    const std::string outputDir = result["output"].as<std::string>();

    // CMake macro creates the output directory before invoking us.

    std::ifstream in(input);
    std::ostringstream ss; ss << in.rdbuf();
    auto od = canopen::parse_eds(ss.str());
    if (!od)
    {
        SPDLOG_ERROR("EDS parse failed.");
        return 4;
    }

    // Derive COB-IDs for RPDO1, RPDO2, TPDO1 (mask control bits)
    auto mask_cobid_base = [](uint64_t v) -> uint32_t { constexpr uint64_t mask29 = 0x1FFFFFFF; return static_cast<uint32_t>(v & mask29); };
    auto get_cobid_base = [&](uint16_t idx) -> uint32_t {
        auto* o = od->get(idx);
        if (!o)
        {
            return 0;
        }
        auto it = o->subs.find(1);
        if (it == o->subs.end() || !it->second.defaultValue)
        {
            return 0;
        }

        const auto& dv = *it->second.defaultValue;
        if (auto pInt = std::get_if<uint64_t>(&dv))
        {
            return mask_cobid_base(*pInt);
        }

        if (auto pExpr = std::get_if<canopen::NodeIdExpr>(&dv))
        {
            return mask_cobid_base(static_cast<uint64_t>(pExpr->constant));
        }

        return 0;
    };

    const uint32_t rpdo1_base = get_cobid_base(0x1400); // expect 0x200
    const uint32_t rpdo2_base = get_cobid_base(0x1401); // expect 0x300
    const uint32_t tpdo1_base = get_cobid_base(0x1800); // expect 0x180

    // Emit helpers with packers (formatted, readable)
    const std::string hdr_path = outputDir + "/" + base + "_helpers.h";
    std::FILE* hdr_out = std::fopen(hdr_path.c_str(), "w");
    if (!hdr_out) throw std::runtime_error("Failed to open output: " + hdr_path);
    fmt::print(hdr_out, "#pragma once\n");
    fmt::print(hdr_out, "#include <cstdint>\n");
    fmt::print(hdr_out, "#include <array>\n");
    fmt::print(hdr_out, "#include \"helpers/can_frame.h\"\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "namespace {}\n", base);
    fmt::print(hdr_out, "{{\n");
    fmt::print(hdr_out, "    using CanFrame = helpers::CanFrame;\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "    // Common object indexes from the EDS\n");
    fmt::print(hdr_out, "    static constexpr uint16_t IDX_DIGITAL_INPUT   = 0x6000;\n");
    fmt::print(hdr_out, "    static constexpr uint16_t IDX_DIGITAL_OUTPUT  = 0x6200;\n");
    fmt::print(hdr_out, "    static constexpr uint16_t IDX_BRIGHTNESS      = 0x6411;\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "    // Resolve COB-IDs for RPDO/TPDO using EDS defaults (falling back to DS401 values)\n");
    fmt::print(hdr_out, "    inline uint32_t cobid_rpdo1(uint8_t node)\n");
    fmt::print(hdr_out, "    {{\n");
    fmt::print(hdr_out, "        return {} + node;\n", (rpdo1_base ? rpdo1_base : 0x200));
    fmt::print(hdr_out, "    }}\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "    inline uint32_t cobid_rpdo2(uint8_t node)\n");
    fmt::print(hdr_out, "    {{\n");
    fmt::print(hdr_out, "        return {} + node;\n", (rpdo2_base ? rpdo2_base : 0x300));
    fmt::print(hdr_out, "    }}\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "    inline uint32_t cobid_tpdo1(uint8_t node)\n");
    fmt::print(hdr_out, "    {{\n");
    fmt::print(hdr_out, "        return {} + node;\n", (tpdo1_base ? tpdo1_base : 0x180));
    fmt::print(hdr_out, "    }}\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "    // Pack indicator bits into RPDO1 (8 bytes)\n");
    fmt::print(hdr_out, "    inline CanFrame pack_rpdo1_indicators(const std::array<uint8_t, 8>& indicators, uint8_t node)\n");
    fmt::print(hdr_out, "    {{\n");
    fmt::print(hdr_out, "        CanFrame f{{}};\n");
    fmt::print(hdr_out, "        f.id  = cobid_rpdo1(node);\n");
    fmt::print(hdr_out, "        f.len = 8;\n");
    fmt::print(hdr_out, "        for (int i = 0; i < 8; ++i)\n");
    fmt::print(hdr_out, "        {{\n");
    fmt::print(hdr_out, "            f.data[i] = indicators[i];\n");
    fmt::print(hdr_out, "        }}\n");
    fmt::print(hdr_out, "        return f;\n");
    fmt::print(hdr_out, "    }}\n");
    fmt::print(hdr_out, "\n");
    fmt::print(hdr_out, "    // Pack brightness values into RPDO2 (Indicator, Backlight)\n");
    fmt::print(hdr_out, "    inline CanFrame pack_rpdo2_brightness(uint16_t indicator, uint16_t backlight, uint8_t node)\n");
    fmt::print(hdr_out, "    {{\n");
    fmt::print(hdr_out, "        CanFrame f{{}};\n");
    fmt::print(hdr_out, "        f.id  = cobid_rpdo2(node);\n");
    fmt::print(hdr_out, "        f.len = 4;\n");
    fmt::print(hdr_out, "        // Little-endian 16-bit values per EDS (0x6411 sub1/sub2)\n");
    fmt::print(hdr_out, "        f.data[0] = static_cast<uint8_t>(indicator & 0xFFu);\n");
    fmt::print(hdr_out, "        f.data[1] = static_cast<uint8_t>((indicator >> 8) & 0xFFu);\n");
    fmt::print(hdr_out, "        f.data[2] = static_cast<uint8_t>(backlight & 0xFFu);\n");
    fmt::print(hdr_out, "        f.data[3] = static_cast<uint8_t>((backlight >> 8) & 0xFFu);\n");
    fmt::print(hdr_out, "        return f;\n");
    fmt::print(hdr_out, "    }}\n");
    fmt::print(hdr_out, "}}\n");
    std::fclose(hdr_out);

    const std::string node_hdr_path = outputDir + "/" + base + "_node.h";
    std::FILE* node_hdr_out = std::fopen(node_hdr_path.c_str(), "w");
    if (!node_hdr_out) throw std::runtime_error("Failed to open output: " + node_hdr_path);
    fmt::print(node_hdr_out, "\n#pragma once\n");
    fmt::print(node_hdr_out, "#include <cstdint>\n");
    fmt::print(node_hdr_out, "#include <functional>\n");
    fmt::print(node_hdr_out, "#include \"helpers/can_frame.h\"\n");
    fmt::print(node_hdr_out, "\n");
    fmt::print(node_hdr_out, "namespace {}\n", base);
    fmt::print(node_hdr_out, "{{\n");
    fmt::print(node_hdr_out, "    // Minimal node wrapper that demultiplexes TPDO1 and exposes button bits.\n");
    fmt::print(node_hdr_out, "    class node\n");
    fmt::print(node_hdr_out, "    {{\n");
    fmt::print(node_hdr_out, "    public:\n");
    fmt::print(node_hdr_out, "        explicit node(uint8_t node_id);\n");
    fmt::print(node_hdr_out, "\n");
    fmt::print(node_hdr_out, "        // Returns true when the frame belonged to this device and was consumed.\n");
    fmt::print(node_hdr_out, "        bool handle_frame(const helpers::CanFrame& frame);\n");
    fmt::print(node_hdr_out, "\n");
    fmt::print(node_hdr_out, "        // Register callback for decoded TPDO1 button bytes (1..8, 9..16, 17..24).\n");
    fmt::print(node_hdr_out, "        void on_buttons(std::function<void(uint8_t, uint8_t, uint8_t)> cb);\n");
    fmt::print(node_hdr_out, "\n");
    fmt::print(node_hdr_out, "    private:\n");
    fmt::print(node_hdr_out, "        uint8_t node_id_;\n");
    fmt::print(node_hdr_out, "        std::function<void(uint8_t, uint8_t, uint8_t)> buttons_cb_;\n");
    fmt::print(node_hdr_out, "    }};\n");
    fmt::print(node_hdr_out, "}}\n");
    std::fclose(node_hdr_out);

    const std::string node_src_path = outputDir + "/" + base + "_node.cpp";
    std::FILE* node_src_out = std::fopen(node_src_path.c_str(), "w");
    if (!node_src_out) throw std::runtime_error("Failed to open output: " + node_src_path);
    fmt::print(node_src_out, "#include \"{}_node.h\"\n", base);
    fmt::print(node_src_out, "#include \"{}_helpers.h\"\n", base);
    fmt::print(node_src_out, "\n");
    fmt::print(node_src_out, "namespace {}\n", base);
    fmt::print(node_src_out, "{{\n");
    fmt::print(node_src_out, "    node::node(uint8_t node_id)\n");
    fmt::print(node_src_out, "        : node_id_(node_id)\n");
    fmt::print(node_src_out, "    {{\n");
    fmt::print(node_src_out, "    }}\n");
    fmt::print(node_src_out, "\n");
    fmt::print(node_src_out, "    void node::on_buttons(std::function<void(uint8_t, uint8_t, uint8_t)> cb)\n");
    fmt::print(node_src_out, "    {{\n");
    fmt::print(node_src_out, "        buttons_cb_ = std::move(cb);\n");
    fmt::print(node_src_out, "    }}\n");
    fmt::print(node_src_out, "\n");
    fmt::print(node_src_out, "    bool node::handle_frame(const helpers::CanFrame& frame)\n");
    fmt::print(node_src_out, "    {{\n");
    fmt::print(node_src_out, "        // Only TPDO1 is consumed here (buttons). Compare 11-bit ID ignoring frame flags.\n");
    fmt::print(node_src_out, "        const uint32_t expected = cobid_tpdo1(node_id_);\n");
    fmt::print(node_src_out, "        if ((frame.id & 0x7FFu) == (expected & 0x7FFu))\n");
    fmt::print(node_src_out, "        {{\n");
    fmt::print(node_src_out, "            if (buttons_cb_)\n");
    fmt::print(node_src_out, "            {{\n");
    fmt::print(node_src_out, "                // Per EDS, first 3 bytes contain buttons 1..24 (8 per byte).\n");
    fmt::print(node_src_out, "                buttons_cb_(frame.data[0], frame.data[1], frame.data[2]);\n");
    fmt::print(node_src_out, "            }}\n");
    fmt::print(node_src_out, "            return true;\n");
    fmt::print(node_src_out, "        }}\n");
    fmt::print(node_src_out, "        return false;\n");
    fmt::print(node_src_out, "    }}\n");
    fmt::print(node_src_out, "}}\n");
    std::fclose(node_src_out);

    // Create an empty helpers.cpp for symmetry
    const std::string helpers_cpp_path = outputDir + "/" + base + "_helpers.cpp";
    std::FILE* helpers_cpp_out = std::fopen(helpers_cpp_path.c_str(), "w");
    if (!helpers_cpp_out) throw std::runtime_error("Failed to open output: " + helpers_cpp_path);
    std::fclose(helpers_cpp_out);

    SPDLOG_INFO("Generated minimal helpers for '{}' at {}", base, outputDir);
    return 0;
}


