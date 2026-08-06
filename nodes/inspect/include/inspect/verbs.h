#ifndef INSPECT_VERBS_H_
#define INSPECT_VERBS_H_

#include "cli/program.h"

// Every verb inspect offers.
//
// TO ADD A VERB: write <name>.cpp with an addXOptions/runX pair, declare them
// here, add one row to kInspectVerbs in main.cpp, and add the file to
// CMakeLists.txt. Nothing else -- the help text, the verb list, the
// `--help`/`help <verb>` handling and the exit codes all derive from that row.
//
// This replaced a hand-written if-chain over argv[1] plus a help string
// maintained by hand in three places, two of which were stale (they listed two
// verbs out of five). See libs/cli/program.h.

namespace inspect
{

// What is on the bus, from advertisements -- so a topic is listed the moment its
// publisher starts, whether or not it has ever sent anything.
void addListOptions(cxxopts::Options& options);
int runList(cli::Context& context);

// Everything known about one key: schema, fields, owner, and optionally traffic.
void addInfoOptions(cxxopts::Options& options);
int runInfo(cli::Context& context);

// Print messages as they arrive, decoded against the schema the publisher named.
void addEchoOptions(cxxopts::Options& options);
int runEcho(cli::Context& context);

// Our processes, by name, joined against what zenoh reports.
void addNodesOptions(cxxopts::Options& options);
int runNodes(cli::Context& context);

// Message rate, with the distribution of the gaps rather than just a count.
void addHzOptions(cxxopts::Options& options);
int runHz(cli::Context& context);

// Bandwidth.
void addBwOptions(cxxopts::Options& options);
int runBw(cli::Context& context);

// Transport delay: arrival minus the publisher's timestamp.
void addLatencyOptions(cxxopts::Options& options);
int runLatency(cli::Context& context);

// A live table of the whole bus: advertised, reachable, owner, rate, bandwidth.
void addWatchOptions(cxxopts::Options& options);
int runWatch(cli::Context& context);

// The schema registry, offline -- the one verb that needs no bus at all.
void addSchemaOptions(cxxopts::Options& options);
int runSchema(cli::Context& context);

// Publish a message built from JSON.
void addPublishOptions(cxxopts::Options& options);
int runPublish(cli::Context& context);

// Callable services, from advertisements.
void addServicesOptions(cxxopts::Options& options);
int runServices(cli::Context& context);

// Call one.
void addCallOptions(cxxopts::Options& options);
int runCall(cli::Context& context);

}  // namespace inspect

#endif  // INSPECT_VERBS_H_
