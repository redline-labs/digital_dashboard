#include "cli/node_options.h"

#include "cli/output.h"
#include "cli/session_options.h"

#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

namespace cli
{

NodeCommandLine::NodeCommandLine(std::string program, std::string description) :
    options_(std::move(program), std::move(description))
{
    options_.add_options()
        ("d,debug", "Debug logging.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("v,verbose", "The same as --debug.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("connect", "Zenoh endpoint to connect to, e.g. tcp/192.168.1.10:7447. Repeatable.",
            cxxopts::value<std::vector<std::string>>())
        ("mode", "Zenoh session mode: peer or client.", cxxopts::value<std::string>())
        ("h,help", "Print usage");
}

NodeCommandLine& NodeCommandLine::withConfig(std::string default_path)
{
    auto value = cxxopts::value<std::string>();
    if (!default_path.empty())
    {
        value->default_value(default_path);
        has_config_default_ = true;
    }
    options_.add_options()("c,config", "Path to the node's YAML config", value);
    has_config_ = true;
    return *this;
}

cxxopts::OptionAdder NodeCommandLine::add()
{
    return options_.add_options();
}

std::optional<int> NodeCommandLine::parse(int argc, char** argv)
{
    try
    {
        result_ = options_.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        SPDLOG_ERROR("Run with --help for usage.");
        return 2;
    }

    if (result_.count("help") != 0)
    {
        out("{}", options_.help());
        return 0;
    }

    debug_ = result_["debug"].as<bool>() || result_["verbose"].as<bool>();
    if (debug_)
    {
        spdlog::set_level(spdlog::level::debug);
    }
    if (has_config_ && (has_config_default_ || result_.count("config") != 0))
    {
        config_ = result_["config"].as<std::string>();
    }

    // Before anything opens a session: SessionManager reads these once.
    applySessionOverrides(result_);
    return std::nullopt;
}

}  // namespace cli
