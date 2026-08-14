// SPDX-License-Identifier: GPL-3.0-or-later

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

Services::Services(const NodeConfig& config, TilesetRegistry& tilesets) :
    mTilesets(tilesets),
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

    SPDLOG_INFO("[node] tiles on '{}', catalog on '{}', assets on '{}'", config.services.tileKey,
                config.services.catalogKey, config.services.assetKey);
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

} // namespace map_server
