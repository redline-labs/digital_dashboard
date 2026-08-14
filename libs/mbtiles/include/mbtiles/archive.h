// SPDX-License-Identifier: GPL-3.0-or-later
//
// A read-only .mbtiles archive.
//
// The mbtiles spec and nothing else: no zenoh, no Qt, no network, no HTTP. The
// same split as libs/gsof against libs/bd992 -- everything here is answerable
// from a file, and nodes/map_server is what puts the answers on the bus.
//
// Two things about the format are quietly wrong by default, and both are
// handled here so no caller has to know:
//
//   * Rows are TMS, requests are XYZ. tile() takes slippy coordinates -- what
//     a style document and every request on the bus use, y increasing
//     southward -- and flips to the
//     TMS row the file stores. That flip happens HERE and nowhere else. A
//     second one further up produces a map that renders beautifully and is
//     mirrored about the equator, which is not a failure anyone reads as a
//     coordinate bug.
//
//   * `tiles` is often a VIEW. The spec's deduplicating layout stores blobs in
//     `images` and the grid in `map`, joined by a view named `tiles`. Querying
//     `tiles` works for both, so nothing here may assume a table -- and the
//     tests build one archive of each shape to keep it that way.
#ifndef MBTILES_ARCHIVE_H
#define MBTILES_ARCHIVE_H

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mbtiles/compression.h"
#include "mbtiles/error.h"
#include "mbtiles/metadata.h"

struct sqlite3;
struct sqlite3_stmt;

namespace mbtiles
{

using Blob = std::vector<std::uint8_t>;

struct Tile
{
    Blob data;
    Encoding encoding { Encoding::Identity };
};

class Archive
{
  public:
    // Open read-only. Fails rather than creating: an .mbtiles that is not there
    // is a configuration mistake, and SQLite's default would helpfully make an
    // empty one and serve blank tiles forever.
    static Result<Archive> open(const std::filesystem::path& path);

    ~Archive();

    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    Archive(Archive&& other) noexcept;
    Archive& operator=(Archive&& other) noexcept;

    const std::filesystem::path& path() const { return mPath; }
    const Metadata& metadata() const { return mMetadata; }

    // One tile, by SLIPPY (XYZ) coordinates.
    //
    // An empty optional means the archive has nothing there, which is the
    // normal answer for most of the tile pyramid and is NOT an error. An Error
    // means the archive itself is broken.
    Result<std::optional<Tile>> tile(std::uint8_t z, std::uint32_t x, std::uint32_t y) const;

    // A TileJSON 2.0.0 document for this archive.
    //
    // `tileUrlTemplate` is substituted in as the sole entry of "tiles", so the
    // caller decides how tiles are addressed -- the server passes a
    // redline://tile/... template and never has to know the widget's URL
    // grammar. The archive's `json` metadata (vector_layers) is merged in at
    // the top level, which is what makes a style's source definition work
    // without any other knowledge of the archive.
    std::string tileJson(std::string_view tileUrlTemplate) const;

  private:
    Archive() = default;

    Result<void> loadMetadata();

    std::filesystem::path mPath;
    sqlite3* mDb { nullptr };
    sqlite3_stmt* mTileStmt { nullptr };
    Metadata mMetadata;

    // The zenoh queryable in nodes/map_server is called concurrently on several
    // RX threads against one Archive. SQLite is built serialized (see
    // third_party/sqlite3.cmake) so the connection survives that, but a single
    // prepared statement has one set of bindings and one cursor -- two threads
    // stepping it at once interleave rows and hand each other the wrong tile.
    // Mutable because tile() is const and reading is conceptually const.
    mutable std::mutex mTileMutex;
};

} // namespace mbtiles

#endif // MBTILES_ARCHIVE_H
