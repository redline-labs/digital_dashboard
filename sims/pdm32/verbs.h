// pdm32_sim's verbs, one row each in main.cpp.
#pragma once

#include "cli/program.h"

namespace pdm32::cli {

void addRunOptions(cxxopts::Options& options);
int runRun(::cli::Context& context);

void addImagesOptions(cxxopts::Options& options);
int runImages(::cli::Context& context);

void addDisasmOptions(cxxopts::Options& options);
int runDisasm(::cli::Context& context);

}  // namespace pdm32::cli
