// SPDX-License-Identifier: GPL-3.0-or-later
#include "graphs.h"

#include <filesystem>

#include <spdlog/spdlog.h>

namespace map_server
{

GraphRegistry::GraphRegistry(const std::vector<GraphConfig>& configured)
{
    for (const GraphConfig& config : configured)
    {
        auto entry = std::make_unique<GraphEntry>();
        entry->name = config.name;
        entry->path = config.path;

        auto opened = road_graph::Graph::open(config.path);
        if (opened)
        {
            SPDLOG_INFO("[graph] {}: {} segments, {} junctions, {} edges", config.name,
                        opened->header().segmentCount, opened->header().nodeCount,
                        opened->header().edgeCount);
            entry->graph = std::make_unique<road_graph::Graph>(std::move(*opened));

            // The overlay, if one has been built. Looked for beside the graph
            // under a fixed name rather than configured: it is derived from the
            // graph, and a config option would only create a way for the two to
            // be pointed at different files.
            //
            // Absent or stale is NOT an error. The plain router answers either
            // way and returns the same route; the overlay only makes it faster.
            // So this logs at info when it is missing and at warn when it is
            // present and refused, because those call for different actions.
            const std::string overlayPath = config.path + ".overlay";
            auto overlay = road_graph::Overlay::open(overlayPath, *entry->graph);
            if (overlay)
            {
                SPDLOG_INFO("[graph] {}: overlay with {} shortcuts over {} transitions",
                            config.name, overlay->header().shortcutCount,
                            overlay->header().originalArcCount);
                entry->overlay = std::make_unique<road_graph::Overlay>(std::move(*overlay));
            }
            else
            {
                entry->overlayError = road_graph::to_string(overlay.error());
                if (std::filesystem::exists(overlayPath))
                {
                    SPDLOG_WARN("[graph] {}: overlay present but unusable: {} (routing will use "
                                "the plain search)",
                                config.name, entry->overlayError);
                }
                else
                {
                    SPDLOG_INFO("[graph] {}: no overlay at {}; routing will use the plain search",
                                config.name, overlayPath);
                }
            }
        }
        else
        {
            // Reported and carried on. One unreadable graph must not take the
            // tile services down with it: a dashboard with a map and no road
            // names is far better than no dashboard.
            entry->error = road_graph::to_string(opened.error());
            SPDLOG_ERROR("[graph] {}: {}", config.name, entry->error);
        }

        mGraphs.push_back(std::move(entry));
    }
}

GraphEntry* GraphRegistry::find(std::string_view name)
{
    return const_cast<GraphEntry*>(static_cast<const GraphRegistry*>(this)->find(name));
}

const GraphEntry* GraphRegistry::find(std::string_view name) const
{
    if (name.empty())
    {
        // A vehicle carries one map. Making every client name it would be
        // ceremony, and the moment there are two the ambiguity is real, so this
        // resolves only when there is exactly one.
        return mGraphs.size() == 1 ? mGraphs.front().get() : nullptr;
    }

    for (const auto& entry : mGraphs)
    {
        if (entry->name == name)
        {
            return entry.get();
        }
    }
    return nullptr;
}

std::size_t GraphRegistry::openCount() const
{
    std::size_t count = 0;
    for (const auto& entry : mGraphs)
    {
        if (entry->graph)
        {
            ++count;
        }
    }
    return count;
}

} // namespace map_server
