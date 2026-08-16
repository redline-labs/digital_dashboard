// SPDX-License-Identifier: GPL-3.0-or-later
#include "mbtiles/archive.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <system_error>
#include <utility>

namespace mbtiles
{
namespace
{

// The largest zoom for which 2^z fits a uint32. z=31 gives 2^31 columns, which
// is already absurd for a map -- OpenStreetMap stops at 19 -- but the point is
// that the shift below stays defined.
constexpr std::uint8_t kMaxZoom = 31;

// The one statement a tile read runs. Shared by open()'s validation probe and
// by every pooled reader, so they cannot describe different queries.
constexpr const char* kTileSql =
    "SELECT tile_data FROM tiles WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?";

// A NULL column reads as empty rather than failing. The metadata table is a
// free-form key/value store and a NULL value is just an absent one.
std::string columnText(sqlite3_stmt* stmt, int column)
{
    const unsigned char* text = sqlite3_column_text(stmt, column);
    if (text == nullptr)
    {
        return {};
    }
    const int bytes = sqlite3_column_bytes(stmt, column);
    return { reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes) };
}

// Zoom values are stored as text and come from outside, so a garbage row must
// not produce a plausible zoom. Anything that is not a whole number in
// [0, kMaxZoom] is refused.
std::optional<std::uint8_t> parseZoom(const std::string& value)
{
    if (value.empty())
    {
        return std::nullopt;
    }

    std::uint32_t accumulated = 0;
    for (const char c : value)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        accumulated = (accumulated * 10U) + static_cast<std::uint32_t>(c - '0');
        if (accumulated > kMaxZoom)
        {
            return std::nullopt;
        }
    }

    return static_cast<std::uint8_t>(accumulated);
}

} // namespace

Result<Archive> Archive::open(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        return not_found(path.string());
    }
    if (std::filesystem::is_directory(path, ec))
    {
        return not_readable(path.string() + " is a directory");
    }

    Archive archive;
    archive.mPath = path;

    // READONLY, and no SQLITE_OPEN_CREATE. Without that flag an absent or
    // unreadable archive would be silently replaced by a new empty database,
    // and the map would render as blank tiles forever with nothing logged.
    // NOMUTEX is deliberately NOT used: see the note on mTileMutex.
    const int rc = sqlite3_open_v2(path.string().c_str(), &archive.mDb, SQLITE_OPEN_READONLY,
                                   nullptr);
    if (rc != SQLITE_OK)
    {
        // sqlite3_open_v2 allocates the handle even when it fails, and the
        // message lives on it, so read it before closing.
        std::string message = (archive.mDb != nullptr) ? sqlite3_errmsg(archive.mDb) : "open failed";
        return not_readable(path.string() + ": " + message, rc);
    }

    // `tiles` may be a table or a view over map+images; this asks SQLite to
    // prepare a statement against it, which succeeds for either and fails for
    // a file that is not an archive at all. Cheaper and more honest than
    // interrogating sqlite_master for a shape we do not actually depend on.
    //
    // Prepared and discarded: it is a VALIDATION here. Tile reads get their
    // own connection and statement from the pool.
    {
        sqlite3_stmt* probe = nullptr;
        const int prepared = sqlite3_prepare_v2(archive.mDb, kTileSql, -1, &probe, nullptr);
        if (prepared != SQLITE_OK)
        {
            return not_an_archive(path.string() + ": no usable `tiles` (" +
                                  sqlite3_errmsg(archive.mDb) + ")");
        }
        sqlite3_finalize(probe);
    }

    if (auto loaded = archive.loadMetadata(); !loaded)
    {
        return std::unexpected(loaded.error());
    }

    return archive;
}

Archive::Reader::~Reader()
{
    if (stmt != nullptr)
    {
        sqlite3_finalize(stmt);
    }
    if (db != nullptr)
    {
        sqlite3_close(db);
    }
}

Archive::~Archive()
{
    // The pool first: every reader owns a connection to the same file.
    mPool.clear();
    if (mDb != nullptr)
    {
        sqlite3_close(mDb);
    }
}

Archive::Archive(Archive&& other) noexcept :
    mPath(std::move(other.mPath)),
    mDb(std::exchange(other.mDb, nullptr)),
    mMetadata(std::move(other.mMetadata)),
    mPool(std::move(other.mPool))
{
    // The mutex is not moved -- a std::mutex cannot be, and there is nothing to
    // carry: an Archive being moved from is not being queried.
}

Archive& Archive::operator=(Archive&& other) noexcept
{
    if (this != &other)
    {
        mPool.clear();
        if (mDb != nullptr)
        {
            sqlite3_close(mDb);
        }

        mPath = std::move(other.mPath);
        mDb = std::exchange(other.mDb, nullptr);
        mMetadata = std::move(other.mMetadata);
        mPool = std::move(other.mPool);
    }
    return *this;
}

Result<void> Archive::loadMetadata()
{
    sqlite3_stmt* stmt = nullptr;
    const int prepared =
        sqlite3_prepare_v2(mDb, "SELECT name, value FROM metadata", -1, &stmt, nullptr);
    if (prepared != SQLITE_OK)
    {
        return not_an_archive(mPath.string() + ": no `metadata` table (" + sqlite3_errmsg(mDb) +
                              ")");
    }

    std::optional<std::uint8_t> minzoom;
    std::optional<std::uint8_t> maxzoom;

    int step = 0;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        std::string key = columnText(stmt, 0);
        std::string value = columnText(stmt, 1);

        if (key == "name")
        {
            mMetadata.name = std::move(value);
        }
        else if (key == "format")
        {
            mMetadata.format = std::move(value);
        }
        else if (key == "version")
        {
            mMetadata.version = std::move(value);
        }
        else if (key == "description")
        {
            mMetadata.description = std::move(value);
        }
        else if (key == "attribution")
        {
            mMetadata.attribution = std::move(value);
        }
        else if (key == "type")
        {
            mMetadata.type = std::move(value);
        }
        else if (key == "json")
        {
            mMetadata.json = std::move(value);
        }
        else if (key == "minzoom")
        {
            minzoom = parseZoom(value);
        }
        else if (key == "maxzoom")
        {
            maxzoom = parseZoom(value);
        }
        else if (key == "bounds")
        {
            // Four numbers or nothing. A partial bounding box is worse than an
            // absent one: it renders, just somewhere else.
            if (auto parsed = parseNumberList(value, 4))
            {
                mMetadata.bounds = std::move(*parsed);
            }
        }
        else if (key == "center")
        {
            if (auto parsed = parseNumberList(value, 3))
            {
                mMetadata.center = std::move(*parsed);
            }
        }
        else
        {
            mMetadata.extra.emplace_back(std::move(key), std::move(value));
        }
    }

    sqlite3_finalize(stmt);

    if (step != SQLITE_DONE)
    {
        return query_error(mPath.string() + ": reading metadata: " + sqlite3_errmsg(mDb), step);
    }

    // Both zoom bounds are optional in the spec, and an archive without them is
    // common enough that refusing it would be wrong. Ask the tiles instead --
    // one indexed aggregate, once, at open.
    if (!minzoom.has_value() || !maxzoom.has_value())
    {
        sqlite3_stmt* zoomStmt = nullptr;
        if (sqlite3_prepare_v2(mDb, "SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles", -1,
                               &zoomStmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(zoomStmt) == SQLITE_ROW &&
                sqlite3_column_type(zoomStmt, 0) != SQLITE_NULL)
            {
                const int lo = sqlite3_column_int(zoomStmt, 0);
                const int hi = sqlite3_column_int(zoomStmt, 1);
                if (!minzoom.has_value() && lo >= 0 && lo <= kMaxZoom)
                {
                    minzoom = static_cast<std::uint8_t>(lo);
                }
                if (!maxzoom.has_value() && hi >= 0 && hi <= kMaxZoom)
                {
                    maxzoom = static_cast<std::uint8_t>(hi);
                }
            }
            sqlite3_finalize(zoomStmt);
        }
    }

    mMetadata.minzoom = minzoom.value_or(0);
    mMetadata.maxzoom = maxzoom.value_or(0);

    // An archive that claims to start deeper than it ends would make every
    // range check below reject everything. Swapping is the only reading of it
    // that leaves the archive usable, and the alternative -- refusing to open
    // -- turns one bad metadata row into no map at all.
    if (mMetadata.minzoom > mMetadata.maxzoom)
    {
        std::swap(mMetadata.minzoom, mMetadata.maxzoom);
    }

    return {};
}

Result<std::optional<Tile>> Archive::tile(std::uint8_t z, std::uint32_t x, std::uint32_t y) const
{
    if (z > kMaxZoom)
    {
        return invalid_argument("zoom " + std::to_string(z) + " is past " +
                                std::to_string(kMaxZoom));
    }

    const std::uint32_t side = 1U << z;
    if (x >= side || y >= side)
    {
        return invalid_argument("tile " + std::to_string(z) + "/" + std::to_string(x) + "/" +
                                std::to_string(y) + " is outside 2^" + std::to_string(z));
    }

    // THE flip. Requests are XYZ (slippy, y increasing southward); the file
    // stores TMS rows (y increasing northward). This is the only place in the
    // tree that converts between them -- see the header.
    const std::uint32_t row = side - 1U - y;

    // A reader of our own for the duration. The lock inside acquire() and
    // release() covers the pool, never the query -- which is the whole point:
    // sharing one statement meant the read itself was serialized.
    Error failure;
    std::unique_ptr<Reader> reader = acquire(failure);
    if (!reader)
    {
        return std::unexpected(failure);
    }

    // Returned to the pool however this exits.
    struct Return
    {
        const Archive* archive;
        std::unique_ptr<Reader>* reader;
        ~Return() { archive->release(std::move(*reader)); }
    } giveBack { this, &reader };

    sqlite3_reset(reader->stmt);
    sqlite3_clear_bindings(reader->stmt);
    sqlite3_bind_int(reader->stmt, 1, static_cast<int>(z));
    sqlite3_bind_int64(reader->stmt, 2, static_cast<sqlite3_int64>(x));
    sqlite3_bind_int64(reader->stmt, 3, static_cast<sqlite3_int64>(row));

    const int step = sqlite3_step(reader->stmt);
    if (step == SQLITE_DONE)
    {
        // No row. Not an error: most of the pyramid is empty.
        sqlite3_reset(reader->stmt);
        return std::optional<Tile> {};
    }
    if (step != SQLITE_ROW)
    {
        const std::string message = sqlite3_errmsg(reader->db);
        sqlite3_reset(reader->stmt);
        return query_error(mPath.string() + ": reading tile: " + message, step);
    }

    Tile tile;
    if (sqlite3_column_type(reader->stmt, 0) != SQLITE_NULL)
    {
        const auto* blob = static_cast<const std::uint8_t*>(sqlite3_column_blob(reader->stmt, 0));
        const int bytes = sqlite3_column_bytes(reader->stmt, 0);
        if (blob != nullptr && bytes > 0)
        {
            tile.data.assign(blob, blob + bytes);
        }
    }

    sqlite3_reset(reader->stmt);

    tile.encoding = sniff(tile.data);
    return std::optional<Tile>(std::move(tile));
}

std::unique_ptr<Archive::Reader> Archive::acquire(Error& error) const
{
    {
        std::unique_lock lock(mPoolMutex);
        // Wait for an idle reader, or for room to open one. Bounded either
        // way: what is being waited for is a 0.015 ms read.
        mPoolFree.wait(lock, [this] { return !mPool.empty() || mReadersOut < kMaxReaders; });

        if (!mPool.empty())
        {
            std::unique_ptr<Reader> reader = std::move(mPool.back());
            mPool.pop_back();
            ++mReadersOut;
            return reader;
        }
        ++mReadersOut;
    }

    // Opened OUTSIDE the lock: a thread waiting for the pool should not also
    // wait for another thread's open().
    auto reader = std::make_unique<Reader>();
    const int rc =
        sqlite3_open_v2(mPath.string().c_str(), &reader->db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        error = not_readable(mPath.string() + ": " +
                             (reader->db != nullptr ? sqlite3_errmsg(reader->db) : "open failed"),
                             rc)
                    .error();
        // The slot has to go back even though nothing is going into the pool,
        // or a run of failed opens wedges every future reader.
        release(nullptr);
        return nullptr;
    }

    if (sqlite3_prepare_v2(reader->db, kTileSql, -1, &reader->stmt, nullptr) != SQLITE_OK)
    {
        error = not_an_archive(mPath.string() + ": no usable `tiles` (" +
                               sqlite3_errmsg(reader->db) + ")")
                    .error();
        release(nullptr);
        return nullptr;
    }

    return reader;
}

void Archive::release(std::unique_ptr<Reader> reader) const
{
    {
        const std::scoped_lock lock(mPoolMutex);
        if (mReadersOut > 0)
        {
            --mReadersOut;
        }
        if (reader)
        {
            mPool.push_back(std::move(reader));
        }
    }
    // Outside the lock: the thread being woken should not immediately block on
    // the mutex the waker still holds.
    mPoolFree.notify_one();
}

std::string Archive::tileJson(std::string_view tileUrlTemplate) const
{
    nlohmann::json doc = nlohmann::json::object();

    // The archive's own `json` column first, so its vector_layers land at the
    // top level where a style expects them. Parsed leniently: an archive with a
    // malformed json column is still a perfectly good source of tiles, and
    // refusing to describe it would take the map down over a metadata typo.
    if (!mMetadata.json.empty())
    {
        auto parsed = nlohmann::json::parse(mMetadata.json, nullptr, false);
        if (parsed.is_object())
        {
            doc = std::move(parsed);
        }
    }

    doc["tilejson"] = "2.0.0";
    doc["scheme"] = "xyz";
    doc["tiles"] = nlohmann::json::array({ std::string(tileUrlTemplate) });
    doc["minzoom"] = mMetadata.minzoom;
    doc["maxzoom"] = mMetadata.maxzoom;

    if (!mMetadata.name.empty())
    {
        doc["name"] = mMetadata.name;
    }
    if (!mMetadata.format.empty())
    {
        doc["format"] = mMetadata.format;
    }
    if (!mMetadata.description.empty())
    {
        doc["description"] = mMetadata.description;
    }
    if (!mMetadata.attribution.empty())
    {
        doc["attribution"] = mMetadata.attribution;
    }
    if (mMetadata.bounds.size() == 4)
    {
        doc["bounds"] = mMetadata.bounds;
    }
    if (mMetadata.center.size() == 3)
    {
        doc["center"] = mMetadata.center;
    }

    return doc.dump();
}

} // namespace mbtiles
