// pdm32_bench: the engines on the real firmware. The delivery scenario's instance (input 1 high from 1 s, output 1
// shorted at 2 s) runs for `--seconds` of sim time in one run_until per engine; reports wall time, simulated bus
// cycles per second, the real-time factor and the JIT's statistics.
//
//   pdm32_bench [--seconds S] [--engine interp|block|jit|all] [--firmware HEX] [--repeat N]
#include <chrono>
#include <cstdio>
#include <string>

#include <spdlog/spdlog.h>

#include "pdm32/top/instance.h"
#include "pdm32/top/scenario.h"
#include "core/core.h"

using namespace sim;
using namespace sim::sys;

int main(int argc, char** argv) {
    core::setupLogging({.program = "pdm32_bench"});
    spdlog::set_level(spdlog::level::warn);
    double seconds = 60;
    std::string engine = "all";
    std::string firmware;
    int repeat = 3;
    for (int k = 1; k + 1 < argc; k += 2) {
        const std::string a = argv[k];
        if (a == "--seconds") {
            seconds = std::stod(argv[k + 1]);
        } else if (a == "--engine") {
            engine = argv[k + 1];
        } else if (a == "--firmware") {
            firmware = argv[k + 1];
        } else if (a == "--repeat") {
            repeat = std::stoi(argv[k + 1]);
        }
    }

    const Scenario scenario = load_scenario(std::string(PDM32_SCENARIO_DIR) + "/short_output.json");
    for (const char* name : {"interp", "block", "jit"}) {
        if (engine != "all" && engine != name) {
            continue;
        }
        double best = 1e9;
        uint64_t cycles = 0;
        uint64_t blocks = 0;
        uint64_t translated = 0;
        uint64_t outside = 0;
        for (int r = 0; r < repeat; r++) {
            InstanceConfig cfg = instance_config_for(scenario, firmware);
            cfg.engine = *parse_engine(name);
            Instance in(cfg);
            prepare_scenario(in, scenario);
            const auto start = std::chrono::steady_clock::now();
            in.run_until_seconds(seconds);
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (wall < best) {
                best = wall;
                cycles = in.m().cpu.cycles;
                if (const auto* jit = in.m().jit()) {
                    blocks = jit->stats.blocks_run;
                    translated = jit->stats.translated;
                    outside = jit->stats.interpreted;
                }
            }
        }
        std::printf("%-7s %6.3f s for %.0f s sim: %6.1f M cycles/s, %6.1fx real time", name, best, seconds,
                    double(cycles) / best / 1e6, seconds / best);
        if (blocks) {
            std::printf("  (%llu blocks run, %llu translated, %llu insns outside)", static_cast<unsigned long long>(blocks),
                        static_cast<unsigned long long>(translated), static_cast<unsigned long long>(outside));
        }
        std::printf("\n");
    }
    return 0;
}
