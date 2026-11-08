// pdm32_sim run | images | disasm.
//
//   run SCENARIO.json [--firmware HEX] [--engine interp|block|jit]
//       runs a scenario, prints its trace and expectations; exit status 0 if all passed, 1 if not
//   images SCENARIO.json --out DIR
//       writes the scenario's synthetic flash blocks (config.bin, current_cal.bin, factory.bin) for inspection
//   disasm [--firmware HEX] [--at ADDR] [--count N]
#include "verbs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>

#include "hc08/disasm.h"
#include "hc08/image.h"
#include "pdm32/top/scenario.h"

namespace pdm32::cli {

namespace {

using namespace sim;
using namespace sim::sys;

void addFirmwareOption(cxxopts::Options& options) {
    options.add_options()("firmware", "The PDM application image (S-records); default $PDM_FIRMWARE_HEX",
                          cxxopts::value<std::string>());
}

void addEngineOption(cxxopts::Options& options) {
    options.add_options()("engine", "CPU engine: interp, block or jit (default jit)", cxxopts::value<std::string>());
}

}  // namespace

void addRunOptions(cxxopts::Options& options) {
    options.add_options()("scenario", "The scenario (JSON)", cxxopts::value<std::string>());
    addFirmwareOption(options);
    addEngineOption(options);
    options.parse_positional({"scenario"});
    options.positional_help("SCENARIO.json");
}

int runRun(::cli::Context& context) {
    const auto path = context.requireString("scenario");
    if (!path) {
        return ::cli::kUsage;
    }
    try {
        Scenario scenario = load_scenario(*path);
        if (context.has("engine")) {
            scenario.engine = parse_engine(context.stringOr("engine", ""));
            if (!scenario.engine) {
                SPDLOG_ERROR("--engine: expected interp, block or jit");
                return ::cli::kUsage;
            }
        }
        Instance instance(instance_config_for(scenario, context.stringOr("firmware", "")));
        prepare_scenario(instance, scenario);
        const ScenarioResult result = run_scenario(instance, scenario);
        std::fputs(format_report(scenario, result).c_str(), stdout);
        return result.passed() ? ::cli::kOk : ::cli::kFailure;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        return ::cli::kFailure;
    }
}

void addImagesOptions(cxxopts::Options& options) {
    options.add_options()("scenario", "The scenario (JSON)", cxxopts::value<std::string>())(
        "out", "Directory to write the blocks into", cxxopts::value<std::string>());
    options.parse_positional({"scenario"});
    options.positional_help("SCENARIO.json");
}

int runImages(::cli::Context& context) {
    const auto path = context.requireString("scenario");
    const auto out = context.requireString("out");
    if (!path || !out) {
        return ::cli::kUsage;
    }
    try {
        const Scenario scenario = load_scenario(*path);
        const InstanceConfig cfg = instance_config_for(scenario, "");
        std::filesystem::create_directories(*out);
        for (const auto& block : cfg.images.blocks) {
            const std::string file = *out + "/" + block.name + ".bin";
            std::ofstream(file, std::ios::binary)
                .write(reinterpret_cast<const char*>(block.bytes.data()), std::streamsize(block.bytes.size()));
            std::printf("%s: $%04X, %zu bytes\n", file.c_str(), block.address, block.bytes.size());
        }
        return ::cli::kOk;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        return ::cli::kFailure;
    }
}

void addDisasmOptions(cxxopts::Options& options) {
    addFirmwareOption(options);
    options.add_options()("at", "Address to start at (0x8000, $8000)", cxxopts::value<std::string>())(
        "count", "Instructions", cxxopts::value<uint64_t>());
}

int runDisasm(::cli::Context& context) {
    try {
        const std::string path = context.stringOr("firmware", default_firmware_path());
        MemoryMap mem;
        mem.add_flash(0, 0x10000);
        load_image(mem, load_srec_file(path));
        std::string at = context.stringOr("at", "0x8000");
        if (!at.empty() && at[0] == '$') {
            at = "0x" + at.substr(1);
        }
        uint16_t pc = uint16_t(std::stoul(at, nullptr, 0));
        const uint64_t count = context.uintOr("count", 20);
        for (uint64_t k = 0; k < count; k++) {
            const Insn insn = decode(pc, [&mem](uint16_t addr) { return mem.bytes[addr]; });
            std::printf("%04X  %s\n", pc, disassemble(insn, pc).c_str());
            pc = uint16_t(pc + insn.length);
        }
        return ::cli::kOk;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        return ::cli::kFailure;
    }
}

}  // namespace pdm32::cli
