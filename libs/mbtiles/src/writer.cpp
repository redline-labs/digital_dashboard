// SPDX-License-Identifier: GPL-3.0-or-later
#include "mbtiles/writer.h"

#include <cstdint>

#include <sqlite3.h>

namespace mbtiles
{
namespace
{

// SQLITE_TRANSIENT is `((sqlite3_destructor_type)-1)`, a C cast, and sqlite3.h
// is deliberately not a SYSTEM include here (see third_party/sqlite3.cmake), so
// the macro trips -Wold-style-cast in our translation units. Same spelling as
// tests/archive_builder.h, which hit this first.
//
// It means "sqlite copies the bytes before returning". Every bind below wants
// that: the strings and spans handed to us are the caller's, and a caller that
// frees between put() and finish() would otherwise write freed memory into the
// archive.
sqlite3_destructor_type sqliteTransient()
{
    return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

Result<void> exec(sqlite3* db, const char* sql)
{
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        const std::string text = message == nullptr ? "unknown" : message;
        sqlite3_free(message);
        return query_error(text, SQLITE_ERROR);
    }
    return {};
}

} // namespace

Writer::Writer(Writer&& other) noexcept :
    mDb(other.mDb),
    mInsert(other.mInsert),
    mMetadata(other.mMetadata),
    mFinished(other.mFinished),
    mTiles(other.mTiles),
    mBytes(other.mBytes)
{
    other.mDb = nullptr;
    other.mInsert = nullptr;
    other.mMetadata = nullptr;
}

Writer& Writer::operator=(Writer&& other) noexcept
{
    if (this != &other)
    {
        this->~Writer();
        new (this) Writer(std::move(other));
    }
    return *this;
}

Writer::~Writer()
{
    if (mInsert != nullptr)
    {
        sqlite3_finalize(mInsert);
    }
    if (mMetadata != nullptr)
    {
        sqlite3_finalize(mMetadata);
    }
    if (mDb != nullptr)
    {
        sqlite3_close(mDb);
    }
}

Result<Writer> Writer::create(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);

    Writer writer;
    if (sqlite3_open(path.c_str(), &writer.mDb) != SQLITE_OK)
    {
        return not_readable("cannot create " + path.string());
    }

    // A tile build writes tens of thousands of rows and is re-runnable from the
    // PBF in minutes, so durability is worth nothing here and costs a great
    // deal: without these, fsync per transaction dominates the build.
    if (auto ok = exec(writer.mDb, "PRAGMA journal_mode = OFF"); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = exec(writer.mDb, "PRAGMA synchronous = OFF"); !ok)
    {
        return std::unexpected(ok.error());
    }

    // The mbtiles 1.3 schema, in its plain form. The deduplicating
    // images/map/tiles-view layout is also legal and Archive reads both; this
    // writes the simple one because a vector tile pyramid has almost no
    // duplicate tiles to save.
    if (auto ok = exec(writer.mDb,
                       "CREATE TABLE metadata (name text, value text);"
                       "CREATE TABLE tiles (zoom_level integer, tile_column integer, "
                       "tile_row integer, tile_data blob);");
        !ok)
    {
        return std::unexpected(ok.error());
    }

    if (auto ok = exec(writer.mDb, "BEGIN"); !ok)
    {
        return std::unexpected(ok.error());
    }

    if (sqlite3_prepare_v2(writer.mDb,
                           "INSERT INTO tiles (zoom_level, tile_column, tile_row, tile_data) "
                           "VALUES (?, ?, ?, ?)",
                           -1, &writer.mInsert, nullptr) != SQLITE_OK)
    {
        return query_error(sqlite3_errmsg(writer.mDb), SQLITE_ERROR);
    }
    if (sqlite3_prepare_v2(writer.mDb, "INSERT INTO metadata (name, value) VALUES (?, ?)", -1,
                           &writer.mMetadata, nullptr) != SQLITE_OK)
    {
        return query_error(sqlite3_errmsg(writer.mDb), SQLITE_ERROR);
    }

    return writer;
}

Result<void> Writer::setMetadata(const std::string& name, const std::string& value)
{
    sqlite3_reset(mMetadata);
    sqlite3_clear_bindings(mMetadata);
    sqlite3_bind_text(mMetadata, 1, name.c_str(), -1, sqliteTransient());
    sqlite3_bind_text(mMetadata, 2, value.c_str(), -1, sqliteTransient());

    if (sqlite3_step(mMetadata) != SQLITE_DONE)
    {
        return query_error(sqlite3_errmsg(mDb), SQLITE_ERROR);
    }
    sqlite3_reset(mMetadata);
    return {};
}

Result<void> Writer::put(std::uint8_t z, std::uint32_t x, std::uint32_t y,
                         std::span<const std::uint8_t> data)
{
    if (z > 30)
    {
        return invalid_argument("zoom " + std::to_string(z) + " is out of range");
    }
    const std::uint32_t side = 1U << z;
    if (x >= side || y >= side)
    {
        return invalid_argument("tile " + std::to_string(z) + "/" + std::to_string(x) + "/" +
                                std::to_string(y) + " is outside 2^" + std::to_string(z));
    }

    // THE flip, the other way round from Archive::tile(). Callers speak XYZ;
    // the file stores TMS. A second flip anywhere in the path yields an archive
    // that renders perfectly, mirrored about the equator -- which nobody reads
    // as a coordinate bug.
    const std::uint32_t row = side - 1U - y;

    sqlite3_reset(mInsert);
    sqlite3_clear_bindings(mInsert);
    sqlite3_bind_int(mInsert, 1, static_cast<int>(z));
    sqlite3_bind_int64(mInsert, 2, static_cast<sqlite3_int64>(x));
    sqlite3_bind_int64(mInsert, 3, static_cast<sqlite3_int64>(row));
    sqlite3_bind_blob64(mInsert, 4, data.data(), data.size(), sqliteTransient());

    if (sqlite3_step(mInsert) != SQLITE_DONE)
    {
        return query_error(sqlite3_errmsg(mDb), SQLITE_ERROR);
    }
    sqlite3_reset(mInsert);

    ++mTiles;
    mBytes += data.size();
    return {};
}

Result<void> Writer::finish()
{
    if (mFinished)
    {
        return {};
    }
    mFinished = true;

    if (auto ok = exec(mDb, "COMMIT"); !ok)
    {
        return ok;
    }

    // After the inserts, not before: maintaining it across fifty thousand rows
    // costs several times what building it once does.
    if (auto ok = exec(mDb, "CREATE UNIQUE INDEX tile_index ON tiles "
                            "(zoom_level, tile_column, tile_row)");
        !ok)
    {
        return ok;
    }
    if (auto ok = exec(mDb, "CREATE UNIQUE INDEX metadata_name ON metadata (name)"); !ok)
    {
        return ok;
    }

    return {};
}

} // namespace mbtiles
