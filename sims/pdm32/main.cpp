// pdm32_sim: the MoTeC PDM32, emulated. See cli.cpp for the verbs.
#include "verbs.h"

#include "cli/program.h"

#include <array>

namespace {

constexpr std::array<cli::Verb, 4> kVerbs{{
    {"serve", "Run the PDM on the bus, its CAN on zenoh, for agents and nodes", pdm32::cli::addServeOptions,
     pdm32::cli::runServe},
    {"run", "Run a scenario and report its expectations", pdm32::cli::addRunOptions, pdm32::cli::runRun},
    {"images", "Write a scenario's synthetic flash blocks", pdm32::cli::addImagesOptions, pdm32::cli::runImages},
    {"disasm", "Disassemble the firmware", pdm32::cli::addDisasmOptions, pdm32::cli::runDisasm},
}};

}  // namespace

int main(int argc, char** argv) {
    const cli::Program program("pdm32_sim", "An emulated MoTeC PDM32 running its own firmware", kVerbs);
    return program.run(argc, argv);
}
