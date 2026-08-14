// SPDX-License-Identifier: GPL-3.0-or-later

#include "tilesets.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace map_server
{

TilesetRegistry::TilesetRegistry(const std::vector<TilesetConfig>& configured)
{
    mTilesets.reserve(configured.size());

    for (const TilesetConfig& entry : configured)
    {
        auto tileset = std::make_unique<Tileset>();
        tileset->name = entry.name;
        tileset->path = entry.path;

        auto opened = mbtiles::Archive::open(entry.path);
        if (opened)
        {
            const mbtiles::Metadata& meta = opened->metadata();
            SPDLOG_INFO("[tileset] {} <- {} ({}, z{}-{}, {})", entry.name, entry.path,
                        meta.format.empty() ? "format unknown" : meta.format, meta.minzoom,
                        meta.maxzoom,
                        meta.json.empty() ? "no layer list" : "layer list present");
            tileset->archive = std::make_unique<mbtiles::Archive>(std::move(*opened));
        }
        else
        {
            tileset->error = mbtiles::to_string(opened.error());
            SPDLOG_ERROR("[tileset] {} <- {}: {}", entry.name, entry.path, tileset->error);
        }

        mTilesets.push_back(std::move(tileset));
    }
}

Tileset* TilesetRegistry::find(std::string_view name)
{
    const auto found = std::find_if(mTilesets.begin(), mTilesets.end(),
                                    [name](const auto& t) { return t->name == name; });
    return (found == mTilesets.end()) ? nullptr : found->get();
}

const Tileset* TilesetRegistry::find(std::string_view name) const
{
    const auto found = std::find_if(mTilesets.begin(), mTilesets.end(),
                                    [name](const auto& t) { return t->name == name; });
    return (found == mTilesets.end()) ? nullptr : found->get();
}

std::size_t TilesetRegistry::openCount() const
{
    return static_cast<std::size_t>(
        std::count_if(mTilesets.begin(), mTilesets.end(),
                      [](const auto& t) { return t->archive != nullptr; }));
}

std::string tileUrlTemplate(const Tileset& tileset)
{
    std::string extension = tileset.archive ? tileset.archive->metadata().format : std::string {};
    if (extension.empty())
    {
        extension = "pbf";
    }

    // Descriptive, not functional. Tiles are fetched by a capnp request naming
    // the tileset and z/x/y -- there is no URL anywhere in the path. TileJSON
    // requires a `tiles` array, so this says what the transport actually is
    // rather than inventing an http:// that nothing would answer.
    return "zenoh://" + tileset.name + "/{z}/{x}/{y}." + extension;
}

} // namespace map_server
