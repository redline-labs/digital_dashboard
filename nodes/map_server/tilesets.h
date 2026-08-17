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

// Whether a coordinate is one this archive could hold, and if not, why not.
//
// Three answers, and keeping them apart is the whole point -- a client needs to
// know whether to give up on a coordinate or to ask for it at a shallower zoom:
//
//   * BadRequest -- not a tile coordinate at all. z past the projection, or x/y
//     outside 2^z. A bug in the client rather than a fact about the archive.
//   * OutOfRange -- a real coordinate, at a zoom level this archive does not
//     go to. The answer exists SHALLOWER, and the client should ask there.
//   * InRange -- the archive covers that level. Whether there is anything at
//     that coordinate is a question for the archive; most of the pyramid is
//     empty, and a miss here is final.
//
// Free and pure so it can be tested without a node, a bus or an archive: this
// is the one decision in the tile path that a client's zoom behaviour hangs
// off, and it is three comparisons that are easy to get quietly backwards.
enum class TileRangeCheck
{
    InRange,
    OutOfRange,
    BadRequest,
};

TileRangeCheck checkTileRange(const mbtiles::Metadata& meta, std::uint8_t z, std::uint32_t x,
                              std::uint32_t y);

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
