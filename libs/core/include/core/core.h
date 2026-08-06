#ifndef CORE_H_
#define CORE_H_

namespace core
{

// The one place the tree's log format lives.
//
// This used to be init_core(argc, argv), which set the pattern *and* parsed the
// command line -- with the program name hardcoded to
// cxxopts::Options("dashboard", "Vehicle instrument cluster."). Its only caller
// was nodes/racegrade_tc8, which therefore printed the dashboard's usage text
// when asked for help. Conflating the two also made it unusable from a
// multi-verb tool, where the verb's options are not known until the verb is.
//
// So this does one thing: the pattern, and the level implied by --debug. The
// caller decides how it learned about --debug; cli::Program does it from the
// global options, a single-purpose node from its own three-line parse.
//
// The pattern is "[date time] [level] [thread:file:line] message", and it is
// worth keeping identical everywhere: logs from the dashboard, the nodes and the
// tools end up interleaved in the same terminal during bring-up, and a
// consistent prefix is what makes that readable.
void setupLogging(bool debug_enabled);

}  // namespace core

#endif  // CORE_H_
