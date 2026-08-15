// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every verb map_build offers.
//
// TO ADD A VERB: write <name>.cpp with an addXOptions/runX pair, declare them
// here, add one row to kVerbs in main.cpp, and add the file to CMakeLists.txt.
// Everything else -- help text, the verb list, `--help`, exit codes -- comes
// from that row. Same shape as nodes/inspect; see libs/cli/program.h.
#ifndef MAP_BUILD_VERBS_H
#define MAP_BUILD_VERBS_H

#include "cli/program.h"

namespace map_build
{

// Read the PBF, classify everything, report, and write nothing.
//
// The fastest way to find out whether a file is what you think it is, and the
// only place the dangling-reference split is visible. Modelled on
// map_server's --check, which docs/map.md calls the fastest way to find out
// whether a path is right.
void addVerifyOptions(cxxopts::Options& options);
int runVerify(cli::Context& context);

// Build the routable road graph.
void addGraphOptions(cxxopts::Options& options);
int runGraph(cli::Context& context);

// Build the vector tile pyramid.
//
// The same extraction the graph verb runs, with the drawn sink attached instead
// of the routable one -- one classify() call decides both, which is what makes
// "the route goes down a road that isn't drawn" impossible rather than merely
// unlikely.
void addTileOptions(cxxopts::Options& options);
int runTile(cli::Context& context);

// Time the router across distance bands.
//
// Stage 6's choice between contraction hierarchies and multi-level Dijkstra is
// supposed to rest on measured numbers rather than on which paper is more
// persuasive. This is where those numbers come from.
void addRouteOptions(cxxopts::Options& options);
int runRoute(cli::Context& context);

// Contract the graph into a routing overlay.
void addOverlayOptions(cxxopts::Options& options);
int runOverlay(cli::Context& context);

} // namespace map_build

#endif // MAP_BUILD_VERBS_H
