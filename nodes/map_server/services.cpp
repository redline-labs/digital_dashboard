// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_rules/classification.h"
#include "road_graph/overlay.h"
#include "road_graph/search.h"

#include "services.h"

#include <spdlog/spdlog.h>

#include <string>

namespace map_server
{
namespace
{

::MapEncoding toWire(mbtiles::Encoding encoding)
{
    switch (encoding)
    {
        case mbtiles::Encoding::Identity:
            return ::MapEncoding::IDENTITY;
        case mbtiles::Encoding::Gzip:
            return ::MapEncoding::GZIP;
        case mbtiles::Encoding::Deflate:
            return ::MapEncoding::DEFLATE;
        case mbtiles::Encoding::Zstd:
            return ::MapEncoding::ZSTD;
    }

    return ::MapEncoding::IDENTITY;
}

void setBlob(capnp::Data::Builder builder, const std::vector<std::uint8_t>& bytes)
{
    if (!bytes.empty())
    {
        std::memcpy(builder.begin(), bytes.data(), bytes.size());
    }
}

} // namespace

Services::Services(const NodeConfig& config, TilesetRegistry& tilesets, GraphRegistry& graphs) :
    mTilesets(tilesets),
    mGraphs(graphs),
    mAssets(config.assets.root, config.assets.maxBytes)
{
    mStatus.emplace(config.services.statusKey);

    mTileService.emplace(config.services.tileKey,
                         [this](const ::MapTileRequest::Reader& request,
                                ::MapTileResponse::Builder& response) {
                             handleTile(request, response);
                         });

    mCatalogService.emplace(config.services.catalogKey,
                            [this](const ::MapCatalogRequest::Reader& request,
                                   ::MapCatalogResponse::Builder& response) {
                                handleCatalog(request, response);
                            });

    mAssetService.emplace(config.services.assetKey,
                          [this](const ::MapAssetRequest::Reader& request,
                                 ::MapAssetResponse::Builder& response) {
                              handleAsset(request, response);
                          });

    mNearestService.emplace(config.services.nearestKey,
                            [this](const ::MapNearestRequest::Reader& request,
                                   ::MapNearestResponse::Builder& response) {
                                handleNearest(request, response);
                            });

    mGraphInfoService.emplace(config.services.graphInfoKey,
                              [this](const ::MapGraphInfoRequest::Reader& request,
                                     ::MapGraphInfoResponse::Builder& response) {
                                  handleGraphInfo(request, response);
                              });

    SPDLOG_INFO("[node] tiles on '{}', catalog on '{}', assets on '{}'", config.services.tileKey,
                config.services.catalogKey, config.services.assetKey);
    mRouteService.emplace(config.services.routeKey,
                          [this](const ::MapRouteRequest::Reader& request,
                                 ::MapRouteResponse::Builder& response) {
                              handleRoute(request, response);
                          });

    SPDLOG_INFO("[node] nearest on '{}', route on '{}', graph info on '{}'",
                config.services.nearestKey, config.services.routeKey,
                config.services.graphInfoKey);
    if (!mAssets.enabled())
    {
        SPDLOG_DEBUG("[node] no asset root configured; the asset service will refuse "
                     "everything. Nothing needs it today -- the map widget renders with "
                     "QPainter and fetches no styles, glyphs or sprites.");
    }
}

void Services::handleTile(const ::MapTileRequest::Reader& request,
                          ::MapTileResponse::Builder& response)
{
    const std::string name = request.getTileset();

    Tileset* tileset = mTilesets.find(name);
    if (tileset == nullptr)
    {
        response.setStatus(::MapStatus::NO_SUCH_TILESET);
        response.setError("no tileset named '" + name + "'");
        return;
    }

    if (!tileset->archive)
    {
        // Configured but unreadable. Deliberately not NO_SUCH_TILESET: the
        // client asked for the right thing and the server is broken, which is
        // a different conversation from a typo.
        response.setStatus(::MapStatus::FAILED);
        response.setError(tileset->error);
        return;
    }

    response.setFormat(tileset->archive->metadata().format);

    auto tile = tileset->archive->tile(request.getZ(), request.getX(), request.getY());
    if (!tile)
    {
        const mbtiles::Error& error = tile.error();
        switch (error.kind)
        {
            case mbtiles::Error::Kind::InvalidArgument:
                response.setStatus(::MapStatus::OUT_OF_RANGE);
                break;
            case mbtiles::Error::Kind::NotFound:
            case mbtiles::Error::Kind::NotReadable:
            case mbtiles::Error::Kind::NotAnArchive:
            case mbtiles::Error::Kind::Query:
                response.setStatus(::MapStatus::FAILED);
                SPDLOG_ERROR("[tile] {}/{}/{}/{}: {}", name, request.getZ(), request.getX(),
                             request.getY(), mbtiles::to_string(error));
                break;
        }
        response.setError(mbtiles::to_string(error));
        return;
    }

    if (!tile->has_value())
    {
        // The common answer. A client asks for whatever is under the viewport
        // and most of the pyramid is empty, so this is not logged.
        tileset->missing.fetch_add(1, std::memory_order_relaxed);
        response.setStatus(::MapStatus::NOT_FOUND);
        return;
    }

    const mbtiles::Tile& found = **tile;
    response.setStatus(::MapStatus::OK);
    response.setEncoding(toWire(found.encoding));
    setBlob(response.initData(static_cast<unsigned>(found.data.size())), found.data);

    tileset->served.fetch_add(1, std::memory_order_relaxed);
    tileset->bytes.fetch_add(found.data.size(), std::memory_order_relaxed);
}

void Services::handleCatalog(const ::MapCatalogRequest::Reader& request,
                             ::MapCatalogResponse::Builder& response)
{
    const std::string wanted = request.getTileset();

    std::vector<const Tileset*> selected;
    if (wanted.empty())
    {
        for (const auto& tileset : mTilesets.all())
        {
            selected.push_back(tileset.get());
        }
    }
    else if (const Tileset* one = mTilesets.find(wanted); one != nullptr)
    {
        selected.push_back(one);
    }
    else
    {
        response.setStatus(::MapStatus::NO_SUCH_TILESET);
        response.setError("no tileset named '" + wanted + "'");
        return;
    }

    auto list = response.initTilesets(static_cast<unsigned>(selected.size()));
    for (unsigned i = 0; i < selected.size(); ++i)
    {
        const Tileset& tileset = *selected[i];
        auto entry = list[i];
        entry.setName(tileset.name);

        if (!tileset.archive)
        {
            // Named, so a picker still shows it, but with nothing to describe.
            // Omitting it entirely would make a broken archive look like a
            // configuration that never mentioned it.
            continue;
        }

        const mbtiles::Metadata& meta = tileset.archive->metadata();
        entry.setFormat(meta.format);
        entry.setMinzoom(meta.minzoom);
        entry.setMaxzoom(meta.maxzoom);
        entry.setAttribution(meta.attribution);
        entry.setDescription(meta.description);

        if (meta.bounds.size() == 4)
        {
            auto bounds = entry.initBounds(4);
            for (unsigned b = 0; b < 4; ++b)
            {
                bounds.set(b, meta.bounds[b]);
            }
        }
        if (meta.center.size() == 3)
        {
            auto center = entry.initCenter(3);
            for (unsigned c = 0; c < 3; ++c)
            {
                center.set(c, meta.center[c]);
            }
        }

        entry.setTileJson(tileset.archive->tileJson(tileUrlTemplate(tileset)));
    }

    response.setStatus(::MapStatus::OK);
}

void Services::handleAsset(const ::MapAssetRequest::Reader& request,
                           ::MapAssetResponse::Builder& response)
{
    const std::string path = request.getPath();

    Asset asset;
    const AssetStatus status = mAssets.load(path, asset);

    switch (status)
    {
        case AssetStatus::Ok:
            response.setStatus(::MapStatus::OK);
            response.setContentType(asset.contentType);
            response.setEncoding(toWire(asset.encoding));
            setBlob(response.initData(static_cast<unsigned>(asset.data.size())), asset.data);
            mAssetsServed.fetch_add(1, std::memory_order_relaxed);
            break;

        case AssetStatus::NotFound:
            // Worth a log line, unlike a missing tile: every asset a style asks
            // for is one the style's author believed was there, so a miss means
            // the map is about to render wrong.
            SPDLOG_WARN("[asset] '{}' not found under {}", path, mAssets.root().string());
            response.setStatus(::MapStatus::NOT_FOUND);
            mAssetsMissing.fetch_add(1, std::memory_order_relaxed);
            break;

        case AssetStatus::Rejected:
            SPDLOG_WARN("[asset] refused '{}': outside the asset root", path);
            response.setStatus(::MapStatus::BAD_REQUEST);
            response.setError("path is outside the asset root");
            mAssetsRejected.fetch_add(1, std::memory_order_relaxed);
            break;

        case AssetStatus::TooLarge:
            response.setStatus(::MapStatus::FAILED);
            response.setError("asset is over the configured size ceiling");
            break;

        case AssetStatus::ReadFailed:
            response.setStatus(::MapStatus::FAILED);
            response.setError("asset could not be read");
            break;

        case AssetStatus::Disabled:
            response.setStatus(::MapStatus::FAILED);
            response.setError("no asset root is configured on this server");
            break;
    }
}

void Services::publishStatus()
{
    if (!mStatus)
    {
        return;
    }

    auto& fields = mStatus->fields();

    const auto& tilesets = mTilesets.all();
    auto list = fields.initTilesets(static_cast<unsigned>(tilesets.size()));
    for (unsigned i = 0; i < tilesets.size(); ++i)
    {
        const Tileset& tileset = *tilesets[i];
        auto entry = list[i];
        entry.setName(tileset.name);
        entry.setPath(tileset.path);
        entry.setOpen(tileset.archive != nullptr);
        entry.setError(tileset.error);
        entry.setTilesServed(tileset.served.load(std::memory_order_relaxed));
        entry.setTilesMissing(tileset.missing.load(std::memory_order_relaxed));
        entry.setBytesServed(tileset.bytes.load(std::memory_order_relaxed));
    }

    fields.setAssetRoot(mAssets.root().string());
    fields.setAssetsServed(mAssetsServed.load(std::memory_order_relaxed));
    fields.setAssetsMissing(mAssetsMissing.load(std::memory_order_relaxed));
    fields.setAssetsRejected(mAssetsRejected.load(std::memory_order_relaxed));

    mStatus->put();
}


// ----------------------------------------------------------------------------
// The road graph
// ----------------------------------------------------------------------------

namespace
{

// map_rules::RouteClass -> the wire vocabulary.
//
// The two enumerations are deliberately parallel and a cast would work today.
// It is written out anyway: a cast would keep compiling after someone inserts a
// value into either one, and the result would be every road on the dash
// reporting the class of its neighbour. Spelled out, -Wswitch-enum makes that
// insertion a build failure here.
::MapRoadClass wireClassOf(map_rules::RouteClass value)
{
    switch (value)
    {
        case map_rules::RouteClass::None:
            return ::MapRoadClass::UNKNOWN;
        case map_rules::RouteClass::Motorway:
            return ::MapRoadClass::MOTORWAY;
        case map_rules::RouteClass::Trunk:
            return ::MapRoadClass::TRUNK;
        case map_rules::RouteClass::Primary:
            return ::MapRoadClass::PRIMARY;
        case map_rules::RouteClass::Secondary:
            return ::MapRoadClass::SECONDARY;
        case map_rules::RouteClass::Tertiary:
            return ::MapRoadClass::TERTIARY;
        case map_rules::RouteClass::Minor:
            return ::MapRoadClass::MINOR;
        case map_rules::RouteClass::Service:
            return ::MapRoadClass::SERVICE;
        case map_rules::RouteClass::Track:
            return ::MapRoadClass::TRACK;
        case map_rules::RouteClass::Path:
            return ::MapRoadClass::PATH;
        case map_rules::RouteClass::Pedestrian:
            return ::MapRoadClass::PEDESTRIAN;
        case map_rules::RouteClass::Ferry:
            return ::MapRoadClass::FERRY;
    }

    // After the switch rather than in a default:, so adding a class stays a
    // compile error.
    return ::MapRoadClass::UNKNOWN;
}

} // namespace

void Services::handleNearest(const MapNearestRequest::Reader& request,
                             MapNearestResponse::Builder& response)
{
    // ON A ZENOH QUERY THREAD, and there is more than one. Nothing below takes
    // a lock: a road_graph::Graph is an mmap'd file that is const after open,
    // so this is genuinely reentrant rather than merely careful.
    const std::string name = request.getGraph().cStr();

    GraphEntry* entry = mGraphs.find(name);
    if (entry == nullptr)
    {
        response.setStatus(::MapQueryStatus::NO_SUCH_GRAPH);
        response.setError(name.empty() ? "no graph is configured" : "no graph named '" + name + "'");
        return;
    }
    if (!entry->graph)
    {
        // Configured and unreadable. A DIFFERENT answer from "not configured",
        // because one is a typo and the other is a permissions or path problem.
        response.setStatus(::MapQueryStatus::NO_SUCH_GRAPH);
        response.setError(entry->error);
        return;
    }

    const double radiusM = request.getRadiusM();
    if (radiusM <= 0.0)
    {
        // Not defaulted. A forgotten field must not silently become a 50 m
        // search that then reports "no road here".
        response.setStatus(::MapQueryStatus::BAD_REQUEST);
        response.setError("radiusM must be greater than zero");
        return;
    }

    entry->queries.fetch_add(1, std::memory_order_relaxed);

    const auto lat = road_graph::fromDegrees(request.getLatitudeDeg());
    const auto lon = road_graph::fromDegrees(request.getLongitudeDeg());

    const road_graph::FileHeader& header = entry->graph->header();
    if (lat < header.south || lat > header.north || lon < header.west || lon > header.east)
    {
        // Past the edge of the map. Distinct from "no road within the radius",
        // because a driver off the end of the coverage should be told so.
        response.setStatus(::MapQueryStatus::OUT_OF_COVERAGE);
        return;
    }

    std::optional<double> heading;
    if (request.getHasHeading())
    {
        heading = request.getHeadingDeg();
    }

    const std::uint16_t wanted = request.getMaxCandidates();
    const auto matches = entry->graph->nearest(lat, lon, radiusM, wanted == 0 ? 1 : wanted, heading);

    if (matches.empty())
    {
        // NORMAL. A car park, a private drive, a field. Not an error, and
        // specifically not the tile meaning of notFound.
        entry->unmatched.fetch_add(1, std::memory_order_relaxed);
        response.setStatus(::MapQueryStatus::NO_MATCH);
        return;
    }

    entry->matched.fetch_add(1, std::memory_order_relaxed);
    response.setStatus(::MapQueryStatus::OK);

    auto list = response.initCandidates(static_cast<unsigned>(matches.size()));
    for (unsigned i = 0; i < matches.size(); ++i)
    {
        const road_graph::Match& match = matches[i];
        const road_graph::SegmentRecord& segment = entry->graph->segments()[match.segment];

        auto out = list[i];
        auto where = out.initWhere();
        where.setSegmentId(segment.id);
        where.setOffsetCm(match.offsetCm);
        // Which way along the segment we are going. With no heading there is no
        // way to know, so the segment's own direction is reported rather than
        // guessed at.
        where.setForward(!heading.has_value() ||
                         road_graph::bearingDeltaDeg(*heading, match.bearingDeg) <= 90.0);

        out.setOsmWayId(segment.osmWayId);
        out.setDistanceM(static_cast<float>(match.distanceM));
        out.setHeadingDeg(static_cast<float>(match.bearingDeg));
        out.setName(std::string(entry->graph->nameOf(segment)));
        out.setRef(std::string(entry->graph->refOf(segment)));
        out.setRoadClass(wireClassOf(static_cast<map_rules::RouteClass>(segment.routeClass)));
        // The raw OSM highway value is not stored in the graph today. Left
        // empty rather than guessed at: an escape hatch that lies is worse than
        // one that is shut.
        out.setHighwayTag("");

        auto speed = out.initSpeed();
        speed.setHasPosted((segment.flags & road_graph::kFlagHasPosted) != 0);
        speed.setPostedKph(segment.postedSpeedKph);
        speed.setPostedSource(static_cast<::MapSpeedSource>(segment.postedSource));
        speed.setFreeFlowKph(segment.freeFlowSpeedKph);
    }
}

void Services::handleGraphInfo(const MapGraphInfoRequest::Reader& request,
                               MapGraphInfoResponse::Builder& response)
{
    const std::string wanted = request.getGraph().cStr();

    std::vector<const GraphEntry*> selected;
    for (const auto& entry : mGraphs.all())
    {
        if (wanted.empty() || entry->name == wanted)
        {
            selected.push_back(entry.get());
        }
    }

    if (!wanted.empty() && selected.empty())
    {
        response.setStatus(::MapQueryStatus::NO_SUCH_GRAPH);
        response.setError("no graph named '" + wanted + "'");
        return;
    }

    response.setStatus(::MapQueryStatus::OK);
    auto list = response.initGraphs(static_cast<unsigned>(selected.size()));
    for (unsigned i = 0; i < selected.size(); ++i)
    {
        const GraphEntry& entry = *selected[i];
        auto out = list[i];
        out.setName(entry.name);
        out.setOpen(entry.graph != nullptr);
        out.setError(entry.error);

        if (!entry.graph)
        {
            continue;
        }

        const road_graph::FileHeader& header = entry.graph->header();
        out.setSegmentCount(header.segmentCount);
        out.setEdgeCount(header.edgeCount);
        out.setBuiltAtUnixS(static_cast<std::uint64_t>(header.builtAtUnixS));

        auto bounds = out.initBounds(4);
        bounds.set(0, header.west * 1e-7);
        bounds.set(1, header.south * 1e-7);
        bounds.set(2, header.east * 1e-7);
        bounds.set(3, header.north * 1e-7);

        // Routing profiles arrive at stage 6 as additive overlay sections. An
        // empty list is the honest answer today.
        out.initProfiles(0);
    }
}


void Services::handleRoute(const MapRouteRequest::Reader& request,
                           MapRouteResponse::Builder& response)
{
    const std::string name = request.getGraph().cStr();

    GraphEntry* entry = mGraphs.find(name);
    if (entry == nullptr || !entry->graph)
    {
        response.setStatus(::MapQueryStatus::NO_SUCH_GRAPH);
        response.setError(entry == nullptr ? "no graph named '" + name + "'" : entry->error);
        return;
    }

    const std::string profile = request.getProfile().cStr();
    if (!profile.empty() && profile != "fastest")
    {
        // Cost profiles are precomputed overlays and arrive at stage 6. Refused
        // by name rather than silently answered with the default, which would
        // give a driver who asked to avoid tolls a route straight over one.
        response.setStatus(::MapQueryStatus::BAD_REQUEST);
        response.setError("profile '" + profile + "' is not built into this graph");
        return;
    }

    const road_graph::Graph& graph = *entry->graph;

    // Snap both endpoints to the network first. A route between two points that
    // are not on roads is not a routing failure, and saying so separately is
    // what lets a caller tell "your destination is in a field" from "there is
    // no way through".
    std::optional<double> fromHeading;
    if (request.getHasFromHeading())
    {
        fromHeading = request.getFromHeadingDeg();
    }

    const auto snap = [&](double lat, double lon, std::optional<double> heading) {
        return graph.nearest(road_graph::fromDegrees(lat), road_graph::fromDegrees(lon), 200.0, 1,
                             heading);
    };

    const auto start = snap(request.getFromLatitudeDeg(), request.getFromLongitudeDeg(), fromHeading);
    const auto finish = snap(request.getToLatitudeDeg(), request.getToLongitudeDeg(), std::nullopt);

    if (start.empty() || finish.empty())
    {
        response.setStatus(::MapQueryStatus::NO_MATCH);
        response.setError(start.empty() ? "no road near the start" : "no road near the destination");
        return;
    }

    const road_graph::SegmentRecord& startSegment = graph.segments()[start[0].segment];
    const road_graph::SegmentRecord& endSegment = graph.segments()[finish[0].segment];

    // THE OVERLAY WHEN THERE IS ONE, the plain search when there is not. Both
    // return the same route -- road_graph_test_contraction requires it, pair by
    // pair -- so this is a speed decision and never an accuracy one, and a
    // vehicle with a stale overlay still gets correct answers.
    auto route = entry->overlay
                     ? road_graph::findRouteVia(graph, *entry->overlay, startSegment.toNode,
                                                endSegment.fromNode)
                     : road_graph::findRoute(graph, startSegment.toNode, endSegment.fromNode);
    if (!route)
    {
        // Genuinely no path, or the search gave up before finding one. Both are
        // noRoute to a caller; the distinction is in the node's log rather than
        // on the wire, because neither gives a driver anything to do.
        response.setStatus(::MapQueryStatus::NO_ROUTE);
        return;
    }

    response.setStatus(::MapQueryStatus::OK);
    response.setDistanceM(route->distanceM);
    response.setDurationS(route->durationS);

    auto ids = response.initSegmentIds(static_cast<unsigned>(route->segments.size()));
    for (unsigned i = 0; i < route->segments.size(); ++i)
    {
        // SEGMENT IDS, not indices. A caller holding the same graph can render
        // from these alone, and an id survives a rebuild where an index does
        // not -- which is the whole of decision 2 in format.h.
        ids.set(i, graph.segments()[route->segments[i]].id);
    }

    // Interleaved lon, lat -- lon first, matching MapTileset.bounds. Full
    // precision, so it will NOT lie exactly on top of the road as drawn from
    // simplified vector tiles at low zoom; drawing the tile feature by
    // segmentId is the exact answer and this is the portable one.
    auto geometry = response.initGeometry(static_cast<unsigned>(route->geometry.size()));
    for (unsigned i = 0; i + 1 < route->geometry.size(); i += 2)
    {
        geometry.set(i, road_graph::toDegrees(route->geometry[i + 1]));
        geometry.set(i + 1, road_graph::toDegrees(route->geometry[i]));
    }
}

} // namespace map_server
