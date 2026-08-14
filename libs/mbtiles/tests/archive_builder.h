// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic .mbtiles archives, built at test time.
//
// Deliberately not a checked-in fixture. The two things worth pinning about
// this library -- the TMS row flip and the map/images VIEW layout -- are
// properties of how an archive is SHAPED, and a builder lets a test state the
// shape it wants in a line. A binary fixture would state it in a comment and
// hope, and there would have to be two of them.
//
// The archives go in a per-test temporary directory that is removed on
// destruction, so a failing test leaves nothing behind and a parallel ctest run
// has no shared path to collide on.
#ifndef MBTILES_TESTS_ARCHIVE_BUILDER_H
#define MBTILES_TESTS_ARCHIVE_BUILDER_H

#include <sqlite3.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mbtiles_test
{

// SQLITE_TRANSIENT is `((sqlite3_destructor_type)-1)`, a C cast, and sqlite3.h
// is deliberately not a SYSTEM include here (see third_party/sqlite3.cmake), so
// the macro trips -Wold-style-cast in our translation units. Spelled once, with
// casts this tree allows.
inline sqlite3_destructor_type sqliteTransient()
{
    return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

// A directory that deletes itself. Named after the test so a leftover from a
// crash says which one crashed.
class TempDir
{
  public:
    explicit TempDir(const std::string& label)
    {
        std::error_code ec;
        mPath = std::filesystem::temp_directory_path(ec) /
                ("mbtiles_test_" + label + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(mPath, ec);
        std::filesystem::create_directories(mPath, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return mPath; }
    std::filesystem::path file(const std::string& name) const { return mPath / name; }

  private:
    std::filesystem::path mPath;
};

// How the archive stores its tiles.
enum class Layout
{
    // The flat form: one `tiles` table. What tilemaker writes.
    Table,
    // The deduplicating form from the spec's appendix: blobs in `images`,
    // grid in `map`, joined by a VIEW named `tiles`. What mb-util writes.
    // Nothing in libs/mbtiles may notice the difference.
    DeduplicatedView,
};

class Builder
{
  public:
    explicit Builder(std::filesystem::path path) : mPath(std::move(path)) {}

    Builder& layout(Layout layout)
    {
        mLayout = layout;
        return *this;
    }

    // Skip creating the metadata table entirely.
    Builder& withoutMetadata()
    {
        mCreateMetadata = false;
        return *this;
    }

    // Skip creating the tiles table entirely.
    Builder& withoutTiles()
    {
        mCreateTiles = false;
        return *this;
    }

    Builder& meta(const std::string& name, const std::string& value)
    {
        mMeta.emplace_back(name, value);
        return *this;
    }

    // Insert a tile at a TMS row -- what the file actually stores. Tests state
    // TMS here and assert in XYZ, which is the only way the flip can be
    // checked rather than merely mirrored on both sides.
    Builder& tmsTile(int z, int column, int row, std::vector<std::uint8_t> data)
    {
        mTiles.push_back({ z, column, row, std::move(data) });
        return *this;
    }

    // Returns an empty string on success, otherwise what went wrong.
    std::string build() const
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(mPath.string().c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            const std::string message = (db != nullptr) ? sqlite3_errmsg(db) : "open failed";
            sqlite3_close(db);
            return message;
        }

        std::string error;
        const auto exec = [&](const std::string& sql) {
            if (!error.empty())
            {
                return;
            }
            char* message = nullptr;
            if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK)
            {
                error = (message != nullptr) ? message : "exec failed";
                sqlite3_free(message);
            }
        };

        if (mCreateMetadata)
        {
            exec("CREATE TABLE metadata (name text, value text, UNIQUE (name))");
            for (const auto& [name, value] : mMeta)
            {
                exec("INSERT INTO metadata (name, value) VALUES ('" + escape(name) + "', '" +
                     escape(value) + "')");
            }
        }

        if (mCreateTiles)
        {
            if (mLayout == Layout::Table)
            {
                exec("CREATE TABLE tiles (zoom_level integer, tile_column integer, "
                     "tile_row integer, tile_data blob)");
                exec("CREATE UNIQUE INDEX tile_index ON tiles "
                     "(zoom_level, tile_column, tile_row)");
            }
            else
            {
                exec("CREATE TABLE map (zoom_level integer, tile_column integer, "
                     "tile_row integer, tile_id text)");
                exec("CREATE TABLE images (tile_data blob, tile_id text)");
                exec("CREATE VIEW tiles AS SELECT map.zoom_level AS zoom_level, "
                     "map.tile_column AS tile_column, map.tile_row AS tile_row, "
                     "images.tile_data AS tile_data FROM map JOIN images "
                     "ON images.tile_id = map.tile_id");
            }

            for (std::size_t i = 0; i < mTiles.size() && error.empty(); ++i)
            {
                error = insertTile(db, mTiles[i], i);
            }
        }

        sqlite3_close(db);
        return error;
    }

  private:
    struct TileRow
    {
        int z {};
        int column {};
        int row {};
        std::vector<std::uint8_t> data;
    };

    static std::string escape(const std::string& value)
    {
        std::string out;
        for (const char c : value)
        {
            if (c == '\'')
            {
                out += '\'';
            }
            out += c;
        }
        return out;
    }

    std::string insertTile(sqlite3* db, const TileRow& tile, std::size_t index) const
    {
        const std::string tileId = "t" + std::to_string(index);

        std::string sql;
        if (mLayout == Layout::Table)
        {
            sql = "INSERT INTO tiles (zoom_level, tile_column, tile_row, tile_data) "
                  "VALUES (?, ?, ?, ?)";
        }
        else
        {
            sql = "INSERT INTO images (tile_id, tile_data) VALUES (?, ?)";
        }

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            return sqlite3_errmsg(db);
        }

        if (mLayout == Layout::Table)
        {
            sqlite3_bind_int(stmt, 1, tile.z);
            sqlite3_bind_int(stmt, 2, tile.column);
            sqlite3_bind_int(stmt, 3, tile.row);
            sqlite3_bind_blob(stmt, 4, tile.data.data(), static_cast<int>(tile.data.size()),
                              sqliteTransient());
        }
        else
        {
            sqlite3_bind_text(stmt, 1, tileId.c_str(), -1, sqliteTransient());
            sqlite3_bind_blob(stmt, 2, tile.data.data(), static_cast<int>(tile.data.size()),
                              sqliteTransient());
        }

        const int step = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE)
        {
            return sqlite3_errmsg(db);
        }

        if (mLayout == Layout::DeduplicatedView)
        {
            sqlite3_stmt* mapStmt = nullptr;
            if (sqlite3_prepare_v2(db,
                                   "INSERT INTO map (zoom_level, tile_column, tile_row, tile_id) "
                                   "VALUES (?, ?, ?, ?)",
                                   -1, &mapStmt, nullptr) != SQLITE_OK)
            {
                return sqlite3_errmsg(db);
            }
            sqlite3_bind_int(mapStmt, 1, tile.z);
            sqlite3_bind_int(mapStmt, 2, tile.column);
            sqlite3_bind_int(mapStmt, 3, tile.row);
            sqlite3_bind_text(mapStmt, 4, tileId.c_str(), -1, sqliteTransient());
            const int mapStep = sqlite3_step(mapStmt);
            sqlite3_finalize(mapStmt);
            if (mapStep != SQLITE_DONE)
            {
                return sqlite3_errmsg(db);
            }
        }

        return {};
    }

    std::filesystem::path mPath;
    Layout mLayout { Layout::Table };
    bool mCreateMetadata { true };
    bool mCreateTiles { true };
    std::vector<std::pair<std::string, std::string>> mMeta;
    std::vector<TileRow> mTiles;
};

// A gzip stream that is a valid container and nothing more -- enough to be
// sniffed, which is all this library ever does with one.
inline std::vector<std::uint8_t> gzipBlob(std::uint8_t marker)
{
    return { 0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, marker };
}

} // namespace mbtiles_test

#endif // MBTILES_TESTS_ARCHIVE_BUILDER_H
