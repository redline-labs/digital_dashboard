// SPDX-License-Identifier: GPL-3.0-or-later
//
// The road graphs this node has open, by the name clients ask for.
//
// Same shape and same rule as TilesetRegistry: a graph that failed to open is
// KEPT, with the reason, so "you asked for something that is not configured"
// and "it is configured and I cannot read it" stay different answers. Dropping
// the broken one makes a permissions problem look like a typo.
//
// Unlike an mbtiles archive, a graph needs no lock. It is an mmap'd file that is
// const after open, so several zenoh query threads read it at once with no
// coordination -- which is why these services fit this node rather than
// straining it.
#ifndef MAP_SERVER_GRAPHS_H
#define MAP_SERVER_GRAPHS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "road_graph/graph.h"
#include "road_graph/overlay.h"

#include "node_config.h"

namespace map_server
{

struct GraphEntry
{
    std::string name;
    std::string path;

    // Null when the file could not be opened; `error` says why.
    std::unique_ptr<road_graph::Graph> graph;
    std::string error;

    // The contraction hierarchy, when one was built and still matches. OPTIONAL
    // BY DESIGN: without it the plain router answers, more slowly, with the same
    // route -- so a missing or stale overlay degrades speed and never accuracy.
    // `overlayError` says why it is absent, because "not built yet" and "built
    // for a different graph" call for different actions.
    std::unique_ptr<road_graph::Overlay> overlay;
    std::string overlayError;

    std::atomic<std::uint64_t> queries { 0 };
    std::atomic<std::uint64_t> matched { 0 };
    std::atomic<std::uint64_t> unmatched { 0 };
};

class GraphRegistry
{
  public:
    explicit GraphRegistry(const std::vector<GraphConfig>& configured);

    // Null when nothing by that name is configured. An EMPTY name resolves to
    // the only configured graph when there is exactly one -- a vehicle carries
    // one map, and making every client name it would be ceremony.
    GraphEntry* find(std::string_view name);
    const GraphEntry* find(std::string_view name) const;

    const std::vector<std::unique_ptr<GraphEntry>>& all() const { return mGraphs; }

    std::size_t openCount() const;

  private:
    std::vector<std::unique_ptr<GraphEntry>> mGraphs;
};

} // namespace map_server

#endif // MAP_SERVER_GRAPHS_H
