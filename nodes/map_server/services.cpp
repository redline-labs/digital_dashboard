// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_rules/classification.h"
#include "map_wire/segment.h"
#include "road_graph/overlay.h"
#include "road_graph/search.h"

#include "route_endpoints.h"
#include "route_geometry.h"

#include "services.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

Services::Services(const NodeConfig& config, TilesetRegistry& tilesets, GraphRegistry& graphs,
                   TracksetRegistry& tracksets) :
    mTilesets(tilesets),
    mGraphs(graphs),
    mTracksets(tracksets),
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

    mTrackCatalogService.emplace(config.services.trackCatalogKey,
                                 [this](const ::MapTrackCatalogRequest::Reader& request,
                                        ::MapTrackCatalogResponse::Builder& response) {
                                     handleTrackCatalog(request, response);
                                 });

    mTrackDetailService.emplace(config.services.trackDetailKey,
                                [this](const ::MapTrackDetailRequest::Reader& request,
                                       ::MapTrackDetailResponse::Builder& response) {
                                    handleTrackDetail(request, response);
                                });

    SPDLOG_INFO("[node] track catalog on '{}', track detail on '{}'",
                config.services.trackCatalogKey, config.services.trackDetailKey);
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

    // The tileset resolved, so the REQUEST is ok however the individual tiles
    // turn out. A batch where every tile is absent is a correct answer about a
    // working archive, and reporting notFound up here would make it look like
    // the tileset was the problem.
    response.setStatus(::MapStatus::OK);
    const mbtiles::Metadata& meta = tileset->archive->metadata();
    response.setFormat(meta.format);

    // On EVERY reply, not just on a mis-ask. A client cannot infer the depth
    // from the per-tile answers -- most of the pyramid is empty, so a hole at
    // z14 and a level the archive never had both read as absent -- and telling
    // it here means the first reply of a session is enough to stop it ever
    // asking out of range. See map_tiles.capnp.
    response.setMinzoom(meta.minzoom);
    response.setMaxzoom(meta.maxzoom);

    const auto wanted = request.getTiles();
    auto results = response.initTiles(wanted.size());

    for (unsigned i = 0; i < wanted.size(); ++i)
    {
        const auto coord = wanted[i];
        auto result = results[i];

        // Echoed back, so a client never has to infer which tile it is holding
        // from position alone.
        auto echo = result.initCoord();
        echo.setZ(coord.getZ());
        echo.setX(coord.getX());
        echo.setY(coord.getY());

        // Checked HERE rather than left to the archive, because the archive
        // cannot tell the two apart: a level it never had and a hole in a level
        // it does have are both "no row", and both would come back notFound.
        // The client needs them separated -- absence is final, out-of-range
        // means ask a level up -- and this is the only place that knows.
        const TileRangeCheck range =
            checkTileRange(meta, coord.getZ(), coord.getX(), coord.getY());
        if (range == TileRangeCheck::BadRequest)
        {
            result.setStatus(::MapStatus::BAD_REQUEST);
            result.setError("not a tile coordinate: " + std::to_string(coord.getZ()) + "/" +
                            std::to_string(coord.getX()) + "/" + std::to_string(coord.getY()));
            continue;
        }
        if (range == TileRangeCheck::OutOfRange)
        {
            result.setStatus(::MapStatus::OUT_OF_RANGE);
            result.setError("zoom " + std::to_string(coord.getZ()) + " is outside " + name +
                            "'s " + std::to_string(meta.minzoom) + "-" +
                            std::to_string(meta.maxzoom));
            continue;
        }

        auto tile = tileset->archive->tile(coord.getZ(), coord.getX(), coord.getY());
        if (!tile)
        {
            const mbtiles::Error& error = tile.error();
            switch (error.kind)
            {
                case mbtiles::Error::Kind::InvalidArgument:
                    // Unreachable now that checkTileRange() screens for it, and
                    // kept as BAD_REQUEST rather than OUT_OF_RANGE so that if
                    // the two ever disagree the client is told it has a bug
                    // instead of politely retrying a level up forever.
                    result.setStatus(::MapStatus::BAD_REQUEST);
                    break;
                case mbtiles::Error::Kind::NotFound:
                case mbtiles::Error::Kind::NotReadable:
                case mbtiles::Error::Kind::NotAnArchive:
                case mbtiles::Error::Kind::Query:
                    result.setStatus(::MapStatus::FAILED);
                    SPDLOG_ERROR("[tile] {}/{}/{}/{}: {}", name, coord.getZ(), coord.getX(),
                                 coord.getY(), mbtiles::to_string(error));
                    break;
            }
            result.setError(mbtiles::to_string(error));
            continue;
        }

        if (!tile->has_value())
        {
            // The common answer. A client asks for whatever is under the
            // viewport and most of the pyramid is empty, so this is not logged.
            tileset->missing.fetch_add(1, std::memory_order_relaxed);
            result.setStatus(::MapStatus::NOT_FOUND);
            continue;
        }

        const mbtiles::Tile& found = **tile;
        result.setStatus(::MapStatus::OK);
        result.setEncoding(toWire(found.encoding));
        setBlob(result.initData(static_cast<unsigned>(found.data.size())), found.data);

        tileset->served.fetch_add(1, std::memory_order_relaxed);
        tileset->bytes.fetch_add(found.data.size(), std::memory_order_relaxed);
    }
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
        out.setRoadClass(map_wire::classOf(segment.routeClass));
        // The raw OSM highway value is not stored in the graph today. Left
        // empty rather than guessed at: an escape hatch that lies is worse than
        // one that is shut.
        out.setHighwayTag("");

        map_wire::fillSpeed(out.initSpeed(), segment);
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

        // The SAME list handleRoute accepts from, so the two cannot disagree.
        const std::vector<std::string> profiles = profilesOf(entry);
        auto names = out.initProfiles(static_cast<unsigned>(profiles.size()));
        for (unsigned p = 0; p < profiles.size(); ++p)
        {
            names.set(p, profiles[p]);
        }
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
    if (!hasProfile(*entry, profile))
    {
        // Refused by name rather than silently answered with the default,
        // which would give a driver who asked to avoid tolls a route straight
        // over one. The list is graphs.cpp's, the same one graphInfo reports.
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

    const RouteEndpoints ends = resolveEndpoints(graph, start[0], finish[0], fromHeading);

    // THE OVERLAY WHEN THERE IS ONE, the plain search when there is not. Both
    // return the same route -- road_graph_test_contraction requires it, pair by
    // pair -- so this is a speed decision and never an accuracy one, and a
    // vehicle with a stale overlay still gets correct answers.
    auto route = entry->overlay
                     ? road_graph::findRouteVia(graph, *entry->overlay, ends.startNode,
                                                ends.endNode)
                     : road_graph::findRoute(graph, ends.startNode, ends.endNode);
    if (!route)
    {
        // Genuinely no path, or the search gave up before finding one. Both are
        // noRoute to a caller; the distinction is in the node's log rather than
        // on the wire, because neither gives a driver anything to do.
        response.setStatus(::MapQueryStatus::NO_ROUTE);
        return;
    }

    // DOOR TO DOOR, not junction to junction. The search runs between the two
    // junctions, so neither the piece of the start segment still ahead of the
    // vehicle nor the piece of the destination segment before the destination
    // was counted -- and on a long rural segment that is hundreds of metres
    // missing from a number a driver reads.
    const auto secondsFor = [](double metres, std::uint16_t speedKph) {
        // A segment with no free-flow speed cannot be timed; contributing zero
        // is better than dividing by it.
        return speedKph == 0 ? 0.0 : metres / (speedKph / 3.6);
    };

    response.setStatus(::MapQueryStatus::OK);
    response.setDistanceM(route->distanceM + ends.startRemainingM + ends.endLeadInM);
    response.setDurationS(route->durationS +
                          secondsFor(ends.startRemainingM, startSegment.freeFlowSpeedKph) +
                          secondsFor(ends.endLeadInM, endSegment.freeFlowSpeedKph));

    auto ids = response.initSegmentIds(static_cast<unsigned>(route->segments.size()));
    for (unsigned i = 0; i < route->segments.size(); ++i)
    {
        // SEGMENT IDS, not indices. A caller holding the same graph can render
        // from these alone, and an id survives a rebuild where an index does
        // not -- which is the whole of decision 2 in format.h.
        ids.set(i, graph.segments()[route->segments[i]].id);
    }

    // Thinned to what the caller asked for BEFORE it goes on the wire. A route
    // is drawn at a zoom and the graph's geometry is finer than any of them;
    // shipping all of it so the client can throw most of it away is the wrong
    // way round. Zero keeps every point.
    const SimplifiedRoute drawn =
        simplifyPerSegment(route->geometry, route->segmentStarts, request.getSimplifyToleranceM());

    // Interleaved lon, lat -- lon first, matching MapTileset.bounds -- in the
    // 1e-7 degrees the graph itself stores. Float64 spent two words per
    // coordinate carrying a number with seven decimal places in it.
    auto geometry = response.initGeometry(static_cast<unsigned>(drawn.geometry.size()));
    for (unsigned i = 0; i + 1 < drawn.geometry.size(); i += 2)
    {
        geometry.set(i, drawn.geometry[i + 1]);
        geometry.set(i + 1, drawn.geometry[i]);
    }

    // Which points belong to which segment. Without it a client holding both
    // lists can draw the line or name the roads but not both.
    auto starts = response.initSegmentStarts(static_cast<unsigned>(drawn.segmentStarts.size()));
    for (unsigned i = 0; i < drawn.segmentStarts.size(); ++i)
    {
        starts.set(i, drawn.segmentStarts[i]);
    }
}

// ============================================================================
// Race tracks
// ============================================================================

namespace
{

::MapTrackQuality qualityOf(track_store::Quality quality)
{
    // Written out rather than static_cast so -Wswitch-enum catches an
    // enumerant added on one side and not the other. Same argument as
    // libs/map_wire: the two vocabularies are allowed to drift only
    // deliberately.
    switch (quality)
    {
        case track_store::Quality::Unknown:
            return ::MapTrackQuality::UNKNOWN;
        case track_store::Quality::Ok:
            return ::MapTrackQuality::OK;
        case track_store::Quality::SeamNotFound:
            return ::MapTrackQuality::SEAM_NOT_FOUND;
        case track_store::Quality::MultipleLoops:
            return ::MapTrackQuality::MULTIPLE_LOOPS;
        case track_store::Quality::WidthOutOfRange:
            return ::MapTrackQuality::WIDTH_OUT_OF_RANGE;
        case track_store::Quality::LengthMismatch:
            return ::MapTrackQuality::LENGTH_MISMATCH;
        case track_store::Quality::SourceLengthImplausible:
            return ::MapTrackQuality::SOURCE_LENGTH_IMPLAUSIBLE;
        case track_store::Quality::Degenerate:
            return ::MapTrackQuality::DEGENERATE;
    }
    return ::MapTrackQuality::UNKNOWN;
}

::MapGateSource gateSourceOf(track_store::GateSource source)
{
    switch (source)
    {
        case track_store::GateSource::None:
            return ::MapGateSource::NONE;
        case track_store::GateSource::DataDrop:
            return ::MapGateSource::DATA_DROP;
        case track_store::GateSource::Derived:
            return ::MapGateSource::DERIVED;
        case track_store::GateSource::Manual:
            return ::MapGateSource::MANUAL;
    }
    return ::MapGateSource::NONE;
}

void fillSummary(const track_store::TrackRecord& record, ::MapTrackSummary::Builder builder)
{
    builder.setId(record.id);
    builder.setName(record.name);
    builder.setCircuit(record.circuit);
    builder.setVenueId(record.venueId);

    auto bounds = builder.initBounds(4);
    bounds.set(0, record.west);
    bounds.set(1, record.south);
    bounds.set(2, record.east);
    bounds.set(3, record.north);

    builder.setCenterlineLengthM(record.centerlineLengthM);
    builder.setPublishedLengthM(record.publishedLengthM);
    builder.setMedianWidthM(record.medianWidthM);
    builder.setPrincipalAxisDeg(record.principalAxisDeg);
    builder.setHasCenterline(record.hasCenterline);
    builder.setClosed(record.closed);
    builder.setCombo(record.combo);
    builder.setQuality(qualityOf(record.quality));
    builder.setOutlinePoints(record.outlinePoints);

    auto gate = builder.initGate();
    gate.setSource(gateSourceOf(record.gate.source));
    gate.setCentreLatE7(record.gate.centreLatE7);
    gate.setCentreLonE7(record.gate.centreLonE7);
    gate.setLeftLatE7(record.gate.leftLatE7);
    gate.setLeftLonE7(record.gate.leftLonE7);
    gate.setRightLatE7(record.gate.rightLatE7);
    gate.setRightLonE7(record.gate.rightLonE7);
    gate.setCenterlineOffsetCm(record.gate.centerlineOffsetCm);
    gate.setWidthM(static_cast<float>(record.gate.widthM));
}

// Metres between two points, on the same equirectangular approximation
// road_graph::distanceM uses. Only ever asked about a track's bbox centre
// against a query point, so the approximation is far inside the noise.
double roughDistanceM(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kMetresPerDegree = 111194.93;
    const double meanLat = (lat1 + lat2) * 0.5 * std::numbers::pi / 180.0;
    const double dLat = (lat2 - lat1) * kMetresPerDegree;
    const double dLon = (lon2 - lon1) * kMetresPerDegree * std::cos(meanLat);
    return std::hypot(dLat, dLon);
}

// The catalogue is stored as interleaved LAT/LON, and the wire is LON/LAT.
//
// Its own function because of how it fails: a track with the two swapped still
// parses, still simplifies and still draws -- somewhere in the Indian Ocean, at
// a latitude that does not exist for half the corpus.
std::vector<std::int32_t> toLonLat(const std::vector<std::int32_t>& latLon)
{
    std::vector<std::int32_t> out(latLon.size());
    for (std::size_t i = 0; i + 1 < latLon.size(); i += 2)
    {
        out[i] = latLon[i + 1];
        out[i + 1] = latLon[i];
    }
    return out;
}

void setCoords(::capnp::List<std::int32_t>::Builder builder,
               const std::vector<std::int32_t>& values)
{
    for (unsigned i = 0; i < values.size(); ++i)
    {
        builder.set(i, values[i]);
    }
}

} // namespace

void Services::handleTrackCatalog(const ::MapTrackCatalogRequest::Reader& request,
                                  ::MapTrackCatalogResponse::Builder& response)
{
    const std::string name = request.getTrackset();
    Trackset* trackset = mTracksets.find(name);
    if (trackset == nullptr)
    {
        response.setStatus(::MapStatus::NO_SUCH_TILESET);
        response.setError("no trackset named '" + name + "'");
        return;
    }
    if (!trackset->store)
    {
        // Configured and unreadable, which is a different conversation from a
        // typo -- most often "this archive has no track tables at all".
        response.setStatus(::MapStatus::FAILED);
        response.setError(trackset->error);
        return;
    }

    trackset->catalogQueries.fetch_add(1, std::memory_order_relaxed);

    response.setStatus(::MapStatus::OK);
    response.setBuildId(trackset->store->buildId());

    const auto near = request.getNear();
    const bool filterByPoint = near.size() >= 2 && request.getRadiusM() > 0.0;
    const double queryLon = filterByPoint ? near[0] : 0.0;
    const double queryLat = filterByPoint ? near[1] : 0.0;
    const std::string venueId = request.getVenueId();

    std::vector<const track_store::TrackRecord*> matched;
    for (const auto& record : trackset->store->tracks())
    {
        if (!venueId.empty() && record.venueId != venueId)
        {
            continue;
        }
        if (filterByPoint)
        {
            // Against the bbox CENTRE, not the nearest edge. A picker asking
            // "what is near me" wants venues, and a circuit is a kilometre
            // across -- the difference is smaller than the question.
            const double centreLat = (record.south + record.north) * 0.5;
            const double centreLon = (record.west + record.east) * 0.5;
            if (roughDistanceM(queryLat, queryLon, centreLat, centreLon) > request.getRadiusM())
            {
                continue;
            }
        }
        matched.push_back(&record);
    }

    auto tracks = response.initTracks(static_cast<unsigned>(matched.size()));
    for (unsigned i = 0; i < matched.size(); ++i)
    {
        fillSummary(*matched[i], tracks[i]);
    }
}

void Services::handleTrackDetail(const ::MapTrackDetailRequest::Reader& request,
                                 ::MapTrackDetailResponse::Builder& response)
{
    const std::string name = request.getTrackset();
    Trackset* trackset = mTracksets.find(name);
    if (trackset == nullptr)
    {
        response.setStatus(::MapStatus::NO_SUCH_TILESET);
        response.setError("no trackset named '" + name + "'");
        return;
    }
    if (!trackset->store)
    {
        response.setStatus(::MapStatus::FAILED);
        response.setError(trackset->error);
        return;
    }

    const std::string id = request.getId();
    if (id.empty())
    {
        response.setStatus(::MapStatus::BAD_REQUEST);
        response.setError("id is required");
        return;
    }

    trackset->detailQueries.fetch_add(1, std::memory_order_relaxed);

    const track_store::TrackRecord* record = trackset->store->find(id);
    if (record == nullptr)
    {
        trackset->missing.fetch_add(1, std::memory_order_relaxed);
        response.setStatus(::MapStatus::NOT_FOUND);
        return;
    }

    response.setStatus(::MapStatus::OK);
    response.setBuildId(trackset->store->buildId());
    fillSummary(*record, response.initSummary());

    const double tolerance = request.getSimplifyToleranceM();

    const auto load = [&](track_store::GeometryKind kind) -> std::vector<std::int32_t> {
        auto blob = trackset->store->geometry(id, kind);
        if (!blob || !blob->has_value())
        {
            return {};
        }
        return toLonLat((*blob)->asCoords());
    };

    if (request.getWantOutline())
    {
        const auto outer = simplifyCoords(load(track_store::GeometryKind::OuterRing), tolerance,
                                          true);
        setCoords(response.initOutlineOuter(static_cast<unsigned>(outer.size())), outer);

        const auto inner = simplifyCoords(load(track_store::GeometryKind::InnerRing), tolerance,
                                          true);
        setCoords(response.initOutlineInner(static_cast<unsigned>(inner.size())), inner);
    }

    if (request.getWantCenterline() && record->hasCenterline)
    {
        auto centerline = trackset->store->geometry(id, track_store::GeometryKind::Centerline);
        auto distance =
            trackset->store->geometry(id, track_store::GeometryKind::CenterlineDistanceCm);
        auto halfWidth = trackset->store->geometry(id, track_store::GeometryKind::HalfWidthCm);

        if (centerline && centerline->has_value())
        {
            // NOT simplified, and that is deliberate. The three lists are
            // PARALLEL -- one distance and one half width per centreline point
            // -- and dropping points from the first without the others silently
            // misaligns every lap measurement made against them. A consumer
            // that only wants to draw the shape has the outline for that.
            const auto points = toLonLat((*centerline)->asCoords());
            setCoords(response.initCenterline(static_cast<unsigned>(points.size())), points);

            if (distance && distance->has_value())
            {
                const auto values = (*distance)->asUint32();
                auto builder = response.initCenterlineDistanceCm(
                    static_cast<unsigned>(values.size()));
                for (unsigned i = 0; i < values.size(); ++i)
                {
                    builder.set(i, values[i]);
                }
            }
            if (halfWidth && halfWidth->has_value())
            {
                const auto values = (*halfWidth)->asUint16();
                auto builder = response.initHalfWidthCm(static_cast<unsigned>(values.size()));
                for (unsigned i = 0; i < values.size(); ++i)
                {
                    builder.set(i, values[i]);
                }
            }
        }
    }
}

} // namespace map_server
