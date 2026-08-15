// SPDX-License-Identifier: GPL-3.0-or-later
//
// The queryables, and the status topic.
//
// Every handler here runs on a zenoh query thread, and there is more than one
// of them. Nothing in this class may assume otherwise: the archives are
// internally locked, the counters are atomics, and the AssetStore is const
// after construction.
#ifndef MAP_SERVER_SERVICES_H
#define MAP_SERVER_SERVICES_H

#include <memory>
#include <optional>

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"

#include "map_graph.capnp.h"
#include "map_tiles.capnp.h"

#include "asset_store.h"
#include "graphs.h"
#include "node_config.h"
#include "tilesets.h"

namespace map_server
{

class Services
{
  public:
    Services(const NodeConfig& config, TilesetRegistry& tilesets, GraphRegistry& graphs);

    // Publish what every tileset is doing. Called from the main loop on a
    // timer, never from a query thread.
    void publishStatus();

  private:
    void handleTile(const MapTileRequest::Reader& request, MapTileResponse::Builder& response);
    void handleCatalog(const MapCatalogRequest::Reader& request,
                       MapCatalogResponse::Builder& response);
    void handleAsset(const MapAssetRequest::Reader& request, MapAssetResponse::Builder& response);

    void handleNearest(const MapNearestRequest::Reader& request,
                       MapNearestResponse::Builder& response);
    void handleGraphInfo(const MapGraphInfoRequest::Reader& request,
                         MapGraphInfoResponse::Builder& response);
    void handleRoute(const MapRouteRequest::Reader& request, MapRouteResponse::Builder& response);

    TilesetRegistry& mTilesets;
    GraphRegistry& mGraphs;
    AssetStore mAssets;

    std::atomic<std::uint64_t> mAssetsServed { 0 };
    std::atomic<std::uint64_t> mAssetsMissing { 0 };
    std::atomic<std::uint64_t> mAssetsRejected { 0 };

    // Declared last, and destroyed first, so a query cannot arrive against a
    // handler whose captured state has already gone.
    std::optional<pub_sub::ZenohPublisher<::MapServerStatus>> mStatus;
    std::optional<pub_sub::ZenohService<::MapTileRequest, ::MapTileResponse>> mTileService;
    std::optional<pub_sub::ZenohService<::MapCatalogRequest, ::MapCatalogResponse>> mCatalogService;
    std::optional<pub_sub::ZenohService<::MapAssetRequest, ::MapAssetResponse>> mAssetService;
    std::optional<pub_sub::ZenohService<::MapNearestRequest, ::MapNearestResponse>> mNearestService;
    std::optional<pub_sub::ZenohService<::MapGraphInfoRequest, ::MapGraphInfoResponse>>
        mGraphInfoService;
    std::optional<pub_sub::ZenohService<::MapRouteRequest, ::MapRouteResponse>> mRouteService;
};

} // namespace map_server

#endif // MAP_SERVER_SERVICES_H
