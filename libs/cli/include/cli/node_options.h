#ifndef CLI_NODE_OPTIONS_H_
#define CLI_NODE_OPTIONS_H_

#include <cxxopts.hpp>

#include <optional>
#include <string>

namespace cli
{

// What every node's command line has in common, declared once so it is spelled
// the same everywhere:
//
//     -c, --config <path>    the node's YAML (only for nodes that take one)
//     -d, --debug            debug logging
//     -v, --verbose          the same switch, which half the nodes called it;
//                            kept so a deployed .args file still works
//     --connect, --mode      the zenoh session, as the CLI tools take them
//     -h, --help             usage on stdout, exit 0
//
// A usage error -- an unknown option, a missing value -- is said on stderr and
// exits 2, the tree's code for "you called it wrong", in every node.
//
//     cli::NodeCommandLine cli("bd992_bridge", "Trimble BD992 to zenoh");
//     cli.withConfig("configs/bd992/bd992.yaml");
//     cli.add()("probe", "List the receiver's application files");
//     if (const auto exit = cli.parse(argc, argv)) return *exit;
//     load(cli.config());
class NodeCommandLine
{
  public:
    NodeCommandLine(std::string program, std::string description);

    // Adds -c/--config. `default_path` empty means no default.
    NodeCommandLine& withConfig(std::string default_path = {});

    // For the node's own options.
    cxxopts::OptionAdder add();

    // Parses, applies --debug and the session options, and returns an exit
    // code when main() should stop now (0 for --help, 2 for a usage error).
    std::optional<int> parse(int argc, char** argv);

    const cxxopts::ParseResult& result() const { return result_; }
    const std::string& config() const { return config_; }
    bool debug() const { return debug_; }
    std::string help() const { return options_.help(); }

  private:
    cxxopts::Options options_;
    cxxopts::ParseResult result_;
    std::string config_;
    bool has_config_ = false;
    bool has_config_default_ = false;
    bool debug_ = false;
};

}  // namespace cli

#endif  // CLI_NODE_OPTIONS_H_
