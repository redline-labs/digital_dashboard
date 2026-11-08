// pdm_flash_dump: read a MoTeC PDM32's flash and RAM over CAN, read-only (session.h says how that is guaranteed).
//
//   pdm_flash_dump dump --channel pcan:0 --out dumps/unit/            a unit on an adapter here
//   pdm_flash_dump dump --channel socketcan:can0 --out dumps/unit/    the same through Linux SocketCAN
//   pdm_flash_dump dump --channel zenoh:vehicle/can0 --out dumps/x/   a bus bridged onto zenoh: can_bridge on a
//                                                                     vehicle, or pdm32_sim serve
//   pdm_flash_dump cal-header dumps/unit/current_cal.bin > sims/pdm32/config/current_cal_dumped.h
//
// The PDM runs at 1 Mbit/s unless its hardware-revision byte says otherwise; --bitrate sets an adapter that is not
// already there. What a dump reads from a unit stays out of the tree (dumps/ is ignored).
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include <spdlog/spdlog.h>

#include "can_backends/registry.h"
#include "cli/output.h"
#include "cli/program.h"
#include "pdm_flash_dump/cal_header.h"
#include "pdm_flash_dump/dump.h"
#include "pdm_flash_dump/session.h"

namespace {

using namespace pdm_flash_dump;

void addDumpOptions(cxxopts::Options& options) {
    options.add_options()("channel", "CAN channel: pcan:0, socketcan:can0, motec:..., zenoh:vehicle/can0",
                          cxxopts::value<std::string>())(
        "out", "Output directory", cxxopts::value<std::string>())(
        "serial", "The unit's serial (else taken from its discovery beacon)", cxxopts::value<uint64_t>())(
        "timeout-ms", "How long to wait for a reply", cxxopts::value<uint64_t>()->default_value("1000"))(
        "bitrate", "Set the adapter to this rate first (bit/s); default: leave it", cxxopts::value<uint64_t>())(
        "with-rom", "Also read the monitor ROM ($FE20-$FF7F)")(
        "range", "Only these ranges, hex, inclusive (1E20-22FF); repeatable",
        cxxopts::value<std::vector<std::string>>());
}

std::optional<std::vector<Range>> planFrom(const ::cli::Context& context) {
    if (!context.has("range")) {
        if (context.flag("with-rom")) {
            const auto all = safe_ranges();
            return std::vector<Range>(all.begin(), all.end());
        }
        return default_plan();
    }
    std::vector<Range> plan;
    for (const std::string& text : context.args()["range"].as<std::vector<std::string>>()) {
        const auto dash = text.find('-');
        try {
            if (dash == std::string::npos) {
                throw std::invalid_argument("no '-'");
            }
            const unsigned long from = std::stoul(text.substr(0, dash), nullptr, 16);
            const unsigned long to = std::stoul(text.substr(dash + 1), nullptr, 16);
            if (to < from || to - from + 1 < 3 || !is_safe(static_cast<uint32_t>(from), static_cast<uint32_t>(to - from + 1))) {
                throw std::invalid_argument("outside RAM, flash and ROM, or under 3 bytes");
            }
            plan.push_back(Range{static_cast<uint16_t>(from), static_cast<uint32_t>(to + 1), "requested"});
        } catch (const std::exception& e) {
            SPDLOG_ERROR("--range {}: {}", text, e.what());
            return std::nullopt;
        }
    }
    return plan;
}

int runDump(::cli::Context& context) {
    const auto channel_id = context.requireString("channel");
    const auto out = context.requireString("out");
    if (!channel_id || !out) {
        return ::cli::kUsage;
    }
    const auto plan = planFrom(context);
    if (!plan) {
        return ::cli::kUsage;
    }
    std::optional<uint16_t> serial;
    if (context.has("serial")) {
        const uint64_t value = context.uintOr("serial", 0);
        if (value > 0xFFFF) {
            SPDLOG_ERROR("--serial: a serial is 16 bits");
            return ::cli::kUsage;
        }
        serial = static_cast<uint16_t>(value);
    }

    can::DefaultRegistryOptions registry_options;
    registry_options.includeZenoh = true;
    const can::Registry registry = can::make_default_registry(registry_options);
    can::OpenOptions open;
    if (context.has("bitrate")) {
        open.bitrate.nominalBps = static_cast<uint32_t>(context.uintOr("bitrate", 1'000'000));
    }
    auto channel = registry.open(*channel_id, open);
    if (!channel) {
        SPDLOG_ERROR("cannot open {}: {}", *channel_id, channel.error().message);
        return ::cli::kFailure;
    }

    ChannelLink link(*channel);
    Session session(link, serial, std::chrono::milliseconds(context.uintOr("timeout-ms", 1000)));
    try {
        if (!session.serial()) {
            SPDLOG_INFO("waiting for the discovery beacon");
            session.discover(std::chrono::seconds(3));
        }
        SPDLOG_INFO("unit serial {}: connecting (read-only session)", *session.serial());
        session.connect();
        const auto started = std::chrono::steady_clock::now();
        const Image image = read_plan(session, *plan, [](const Range& range) {
            SPDLOG_INFO("  ${:04X}-${:04X}  {}", range.from, range.to - 1, range.name);
        });
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto manifest = write_dump(session, image, *plan, seconds, *out);
        session.close_session();

        for (const char* name : {"config", "current_cal"}) {
            const auto& check = manifest[name];
            ::cli::out("{}: {}", name,
                       !check.value("header_ok", false) ? std::string("not present or not valid")
                       : check.value("crc_ok", false)
                           ? fmt::format("CRC ok, {} bytes", check["length"].get<uint32_t>())
                           : std::string("CRC BAD"));
        }
        ::cli::out("{} bytes in {:.1f} s ({} reads retried): {}/image.s19", image.size(), seconds,
                   session.retries(), *out);
        return ::cli::kOk;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        return ::cli::kFailure;
    }
}

void addCalHeaderOptions(cxxopts::Options& options) {
    options.add_options()("block", "current_cal.bin from a dump", cxxopts::value<std::string>())(
        "name", "The constant's suffix (kCurrentCal<Name>)", cxxopts::value<std::string>()->default_value("dumped"));
    options.parse_positional({"block"});
    options.positional_help("current_cal.bin");
}

int runCalHeader(::cli::Context& context) {
    const auto path = context.requireString("block");
    if (!path) {
        return ::cli::kUsage;
    }
    std::ifstream in(*path, std::ios::binary);
    if (!in) {
        SPDLOG_ERROR("cannot read {}", *path);
        return ::cli::kFailure;
    }
    const std::vector<uint8_t> block((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        const std::string source = std::filesystem::path(*path).filename().string();
        std::string header = current_cal_header(block, source, context.stringOr("name", "dumped"));
        header.pop_back();  // cli::out ends the line itself
        ::cli::out("{}", header);
        return ::cli::kOk;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}: {}", *path, e.what());
        return ::cli::kFailure;
    }
}

constexpr std::array<::cli::Verb, 2> kVerbs{{
    {"dump", "Read a PDM32's flash and RAM, read-only", addDumpOptions, runDump},
    {"cal-header", "Turn a dumped current calibration into sims/pdm32's C++ header", addCalHeaderOptions,
     runCalHeader},
}};

}  // namespace

int main(int argc, char** argv) {
    const ::cli::Program program("pdm_flash_dump", "Read a MoTeC PDM32 over CAN, read-only", kVerbs);
    return program.run(argc, argv);
}
