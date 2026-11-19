// m150_sim's verbs, one row each in main.cpp.
#pragma once

#include "cli/program.h"

namespace m150::cli {

void addServeOptions(cxxopts::Options& options);
int runServe(::cli::Context& context);

void addRunOptions(cxxopts::Options& options);
int runRun(::cli::Context& context);

}  // namespace m150::cli
