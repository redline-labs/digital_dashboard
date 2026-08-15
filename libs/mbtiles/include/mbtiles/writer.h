// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writing an .mbtiles archive.
//
// The counterpart of archive.h, and it repeats that header's one rule: THE
// ARCHIVE STORES TMS ROWS AND EVERYTHING ELSE IN THIS TREE USES XYZ. The flip
// happens in exactly two places -- Archive::tile() on the way out and put() on
// the way in -- and a third one anywhere would produce a map that renders
// beautifully, mirrored about the equator.
//
// This is used ONLY by tools/map_build, which never ships. It lives here rather
// than in the tool because the schema and the flip belong with the format, and
// because writing an archive the reader cannot read is the failure this
// arrangement makes impossible: the round-trip test drives both.
#ifndef MBTILES_WRITER_H
#define MBTILES_WRITER_H

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "mbtiles/error.h"

struct sqlite3;
struct sqlite3_stmt;

namespace mbtiles
{

class Writer
{
  public:
    // Creates the file, replacing any existing one.
    //
    // Replacing rather than appending is deliberate: a tile build is
    // all-or-nothing, and a half-updated archive with tiles from two different
    // OSM extracts is a map that is subtly inconsistent with itself.
    static Result<Writer> create(const std::filesystem::path& path);

    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();

    // A metadata row. `name`/`format`/`minzoom`/`maxzoom`/`bounds`/`center`
    // and the `json` vector_layers list are what every client reads.
    Result<void> setMetadata(const std::string& name, const std::string& value);

    // One tile, in XYZ. `data` is stored exactly as given -- gzipping is the
    // caller's decision, because the server passes tiles through untouched and
    // so whatever goes in here is what a client will have to inflate.
    Result<void> put(std::uint8_t z, std::uint32_t x, std::uint32_t y,
                     std::span<const std::uint8_t> data);

    // Commit, index, and close.
    //
    // The index is created AFTER the tiles rather than before: building it
    // incrementally over fifty thousand inserts costs several times what
    // building it once at the end does, and nothing queries the archive while
    // it is being written.
    Result<void> finish();

    std::uint64_t tilesWritten() const { return mTiles; }
    std::uint64_t bytesWritten() const { return mBytes; }

  private:
    Writer() = default;

    sqlite3* mDb { nullptr };
    sqlite3_stmt* mInsert { nullptr };
    sqlite3_stmt* mMetadata { nullptr };
    bool mFinished { false };

    std::uint64_t mTiles { 0 };
    std::uint64_t mBytes { 0 };
};

} // namespace mbtiles

#endif // MBTILES_WRITER_H
