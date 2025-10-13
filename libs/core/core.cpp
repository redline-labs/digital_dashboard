#include "core/core.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>


namespace core
{

void init_core(int argc, char** argv)
{
    //size_t max_size_bytes = 5u * 1024u * 1024u;  // 5MB
    //size_t max_files = 3u;
    //auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/rotating.txt", max_size_bytes, max_files, true);
    //spdlog::default_logger()->sinks().push_back(file_sink);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");


    cxxopts::Options options("dashboard", "Vehicle instrument cluster.");

    //options.add_options("required")
    //    ("c,config", "Path to YAML configuration file.", cxxopts::value<std::string>());

    options.add_options("optional")
        ("debug", "Enable debug logging.", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("h,help", "Print usage");

    auto args_result = options.parse(argc, argv);

    // Set the logging level based on the debug flag
    bool debug_enabled = args_result["debug"].as<bool>();
    spdlog::set_level(debug_enabled ? spdlog::level::debug : spdlog::level::info);
}

}  // namespace core
