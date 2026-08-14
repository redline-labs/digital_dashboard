// SPDX-License-Identifier: GPL-3.0-or-later
//
// The archives this node has open, by the name clients ask for.
//
// Deliberately does NOT drop a tileset whose archive failed to open. A missing
// name and a broken archive want different answers -- "you asked for something
// that is not configured" against "it is configured and I cannot read it" --
// and collapsing them makes a permissions problem look like a typo.
#ifndef MAP_SERVER_TILESETS_H
#define MAP_SERVER_TILESETS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mbtiles/archive.h"

#include "node_config.h"

namespace map_server
{

struct Tileset
{
    std::string name;
    std::string path;

    // Empty when the archive could not be opened; `error` says why.
    std::unique_ptr<mbtiles::Archive> archive;
    std::string error;

    // Counters, written from zenoh query threads.
    std::atomic<std::uint64_t> served { 0 };
    std::atomic<std::uint64_t> missing { 0 };
    std::atomic<std::uint64_t> bytes { 0 };
};

class TilesetRegistry
{
  public:
    // Opens every configured archive. Reports failures and carries on: one
    // unreadable archive must not take the others down with it, because the
    // dashboard would then show no map at all rather than one map.
    explicit TilesetRegistry(const std::vector<TilesetConfig>& configured);

    // Null when nothing by that name is configured.
    Tileset* find(std::string_view name);
    const Tileset* find(std::string_view name) const;

    const std::vector<std::unique_ptr<Tileset>>& all() const { return mTilesets; }

    // How many opened. Zero with a non-empty configuration is worth saying out
    // loud at startup.
    std::size_t openCount() const;

  private:
    std::vector<std::unique_ptr<Tileset>> mTilesets;
};

// The tile URL template a client should use for this tileset, as it appears in
// the TileJSON. The one place the wire URL grammar is written down on the server
// side; dashboard/widgets/map/tile_url.h is its counterpart.
std::string tileUrlTemplate(const Tileset& tileset);

} // namespace map_server

#endif // MAP_SERVER_TILESETS_H
