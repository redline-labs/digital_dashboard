// pdm32_sim serve | run | images | disasm.
//
//   serve [-c CONFIG] [--mcp[=SOCK]] [--scenario S.json] [--firmware HEX] [--engine E] [--stepped] [--allow-write]
//       the PDM on the bus (server.h); with --mcp, prints "AGENT_READY <socket> <pid>" once it is serving
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

#include "agent_control/log_ring.h"
#include "cli/interrupt.h"
#include "agent_control/mcp_option.h"
#include "hc08/disasm.h"
#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"
#include "server.h"
#include "serve_config.h"
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

void addServeOptions(cxxopts::Options& options) {
    options.add_options()("c,config", "The serve config (YAML); see serve_config.h", cxxopts::value<std::string>())(
        "scenario", "A scenario to set up the PDM's loads, inputs and configuration", cxxopts::value<std::string>())(
        "stepped", "Time moves only when an agent says (run, step), not with the wall clock")(
        "allow-write", "Let agents write firmware memory");
    addFirmwareOption(options);
    addEngineOption(options);
    agent_control::addMcpOption(options, "pdm32_sim");
}

int runServe(::cli::Context& context) {
    ServeOptions options;
    if (context.has("config")) {
        auto config = load_serve_config(context.stringOr("config", ""));
        if (!config) {
            return ::cli::kFailure;
        }
        options.config = std::move(*config);
    }
    // The command line wins over the file.
    options.config.scenario = context.stringOr("scenario", options.config.scenario);
    options.config.firmware = context.stringOr("firmware", options.config.firmware);
    options.config.engine = context.stringOr("engine", options.config.engine);
    options.config.stepped = options.config.stepped || context.flag("stepped");
    options.allow_write = context.flag("allow-write");
    if (const auto socket = agent_control::mcpSocketPath(context.args(), "pdm32_sim")) {
        options.control_socket = *socket;
        agent_control::installLogRing();
    }

    pub_sub::NodeIdentity identity("pdm32_sim");
    node_health::HealthReporter health("pdm32_sim");
    ::cli::installInterruptHandler();

    std::unique_ptr<Server> server;
    try {
        server = std::make_unique<Server>(options);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        return ::cli::kFailure;
    }

    server->run(
        [&]() {
            const ServeConfig& config = server->options().config;
            SPDLOG_INFO("[sim] pdm32 on '{}' (tx '{}'), {} time", config.can.rxKey, config.can.txKey,
                        config.stepped ? "stepped" : "real");
            health.markReady();
            if (!options.control_socket.empty()) {
                agent_control::announceReady(options.control_socket);
            }
        },
        [&]() { health.kick(); });
    SPDLOG_INFO("[sim] stopping");
    return ::cli::kOk;
}

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
