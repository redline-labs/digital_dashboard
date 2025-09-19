#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <string>
#include <vector>

#include "dump.h"

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    if (argc < 2) {
        SPDLOG_INFO("Usage: inspect <verb> [options]\n  Verbs:\n    dump   Subscribe and print payloads\n");
        return 0;
    }

    // Extract verb and forward remaining args to verb handler.
    const std::string verb = argv[1];

    // Build argv slice for the verb: program name becomes "inspect <verb>"
    // We will pass (argc-1, argv+1) so that verb handler sees its own argv[0] as the verb.
    int verb_argc = argc - 1;
    char** verb_argv = argv + 1;

    if (verb == "dump") {
        return run_dump(verb_argc, verb_argv);
    }

    SPDLOG_ERROR("Unknown verb: '{}'", verb);
    SPDLOG_INFO("Available verbs: dump");
    return 1;
}


