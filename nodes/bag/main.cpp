#include "bag_tool/verbs.h"

#include "cli/program.h"

#include <array>

namespace
{

constexpr std::array<cli::Verb, 5> kBagVerbs{{
    {"record", "Subscribe to the bus and write a recording", bag_tool::addRecordOptions,
     bag_tool::runRecord},
    {"info", "What is in a recording, read from its index", bag_tool::addInfoOptions,
     bag_tool::runInfo},
    {"play", "Republish a recording with its original timing", bag_tool::addPlayOptions,
     bag_tool::runPlay},
    {"verify", "Check a recording is structurally valid MCAP", bag_tool::addVerifyOptions,
     bag_tool::runVerify},
    {"reindex", "Rebuild metadata.yaml from the parts on disk", bag_tool::addReindexOptions,
     bag_tool::runReindex},
}};

}  // namespace

int main(int argc, char** argv)
{
    const cli::Program program("bag", "Record and replay the zenoh bus", kBagVerbs);
    return program.run(argc, argv);
}
