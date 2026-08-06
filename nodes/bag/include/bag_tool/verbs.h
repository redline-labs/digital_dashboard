#ifndef BAG_TOOL_VERBS_H_
#define BAG_TOOL_VERBS_H_

#include "cli/program.h"

// The `bag` tool's verbs.
//
// Same shape as nodes/inspect: one file per verb, an addXOptions/runX pair
// declared here, one row in the table in main.cpp. See libs/cli/program.h.

namespace bag_tool
{

// Subscribe and write. The only verb that touches the bus for input.
void addRecordOptions(cxxopts::Options& options);
int runRecord(cli::Context& context);

// What is in a recording, from the index alone -- no message is read.
void addInfoOptions(cxxopts::Options& options);
int runInfo(cli::Context& context);

// Republish a recording onto the bus with its original timing.
void addPlayOptions(cxxopts::Options& options);
int runPlay(cli::Context& context);

// Check that a recording is structurally valid MCAP, against the spec and
// without mcap's own reader. This is `mcap doctor` with no dependency.
void addVerifyOptions(cxxopts::Options& options);
int runVerify(cli::Context& context);

// Rebuild metadata.yaml from the parts on disk, for a recording whose recorder
// was killed.
void addReindexOptions(cxxopts::Options& options);
int runReindex(cli::Context& context);

}  // namespace bag_tool

#endif  // BAG_TOOL_VERBS_H_
