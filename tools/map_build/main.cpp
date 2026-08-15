// SPDX-License-Identifier: GPL-3.0-or-later
//
// map_build: an OSM PBF in, the things the map stack needs out.
//
// WORKSTATION ONLY. It never runs on the vehicle, which is why it lives in
// tools/ rather than nodes/ -- see the note in AGENTS.md. It links zlib and
// holds tens of gigabytes of scratch state at continental scale, none of which
// has any business in a vehicle image, and the directory boundary is what keeps
// it out of a deployment manifest.

#include <array>

#include <spdlog/spdlog.h>

#include "cli/program.h"
#include "map_build/verbs.h"

namespace
{

constexpr std::array<cli::Verb, 5> kVerbs { {
    { "verify", "Read a PBF, classify everything, report, write nothing",
      map_build::addVerifyOptions, map_build::runVerify },
    { "graph", "Build the routable road graph", map_build::addGraphOptions,
      map_build::runGraph },
    { "tile", "Build the vector tile pyramid", map_build::addTileOptions, map_build::runTile },
    { "route", "Time the router across distance bands", map_build::addRouteOptions,
      map_build::runRoute },
    { "overlay", "Contract the graph into a routing overlay", map_build::addOverlayOptions,
      map_build::runOverlay },
} };

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    const cli::Program program("map_build", "Build map artifacts from an OSM PBF", kVerbs);
    return program.run(argc, argv);
}
