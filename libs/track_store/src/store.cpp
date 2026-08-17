// SPDX-License-Identifier: GPL-3.0-or-later
#include "track_store/store.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

#include <sqlite3.h>

namespace track_store
{

namespace
{

// Bumped only when the blob layout changes in a way an old reader would decode
// wrongly. A reader refuses what it does not recognise rather than reading it
// as garbage -- the failure otherwise is a centreline in the wrong place, which
// draws perfectly.
constexpr std::uint8_t kBlobVersion = 1;
constexpr std::size_t kBlobHeaderBytes = 4;

// SQLITE_TRANSIENT and SQLITE_STATIC are C casts inside macros, and sqlite3.h
// is deliberately not a SYSTEM include (see third_party/sqlite3.cmake), so both
// trip -Wold-style-cast in our translation units. Same spelling as
// libs/mbtiles/src/writer.cpp, which hit this first.
//
// TRANSIENT means "sqlite copies the bytes before returning", which is what
// every bind here wants: the strings and spans are the caller's, and a caller
// that frees before finish() would otherwise write freed memory into the file.
// STATIC is only ever used for string literals.
sqlite3_destructor_type sqliteTransient()
{
    return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

sqlite3_destructor_type sqliteStatic()
{
    return nullptr;
}

constexpr const char* kBuildIdKey = "build_id";
constexpr const char* kSchemaVersionKey = "schema_version";
constexpr const char* kSchemaVersion = "1";

std::unexpected<Error> fail(Error::Kind kind, std::string message, int code = 0)
{
    return std::unexpected(Error { kind, std::move(message), code });
}

std::size_t valueWidth(GeometryKind kind)
{
    switch (kind)
    {
        case GeometryKind::OuterRing:
        case GeometryKind::InnerRing:
        case GeometryKind::Centerline:
            return sizeof(std::int32_t);
        case GeometryKind::CenterlineDistanceCm:
            return sizeof(std::uint32_t);
        case GeometryKind::HalfWidthCm:
            return sizeof(std::uint16_t);
    }
    return 0;
}

template <typename T>
std::vector<std::uint8_t> encode(GeometryKind kind, std::span<const T> values)
{
    std::vector<std::uint8_t> out(kBlobHeaderBytes + values.size() * sizeof(T));
    out[0] = kBlobVersion;
    out[1] = static_cast<std::uint8_t>(kind);
    out[2] = 0;
    out[3] = 0;
    if (!values.empty())
    {
        std::memcpy(out.data() + kBlobHeaderBytes, values.data(), values.size() * sizeof(T));
    }
    return out;
}

template <typename T>
std::vector<T> decode(const std::vector<std::uint8_t>& data)
{
    if (data.size() < kBlobHeaderBytes)
    {
        return {};
    }
    const std::size_t payload = data.size() - kBlobHeaderBytes;
    if (payload % sizeof(T) != 0)
    {
        // A trailing partial value means the blob is not what it claims. Half
        // a coordinate decoded as a whole one is a point in the sea.
        return {};
    }
    std::vector<T> out(payload / sizeof(T));
    if (!out.empty())
    {
        std::memcpy(out.data(), data.data() + kBlobHeaderBytes, payload);
    }
    return out;
}

std::string textOr(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite3_column_text(statement, column);
    if (text == nullptr)
    {
        return {};
    }
    return reinterpret_cast<const char*>(text);
}

Result<void> exec(sqlite3* db, const char* sql)
{
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK)
    {
        std::string what = message == nullptr ? "sqlite3_exec failed" : message;
        sqlite3_free(message);
        return fail(Error::Kind::Query, std::move(what), rc);
    }
    return {};
}

bool hasTable(sqlite3* db, const char* name)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
                           &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    sqlite3_bind_text(statement, 1, name, -1, sqliteStatic());
    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return found;
}

std::optional<std::string> readMeta(sqlite3* db, const char* table, const char* key)
{
    const std::string sql = std::string("SELECT value FROM ") + table + " WHERE name=?";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        return std::nullopt;
    }
    sqlite3_bind_text(statement, 1, key, -1, sqliteStatic());
    std::optional<std::string> out;
    if (sqlite3_step(statement) == SQLITE_ROW)
    {
        out = textOr(statement, 0);
    }
    sqlite3_finalize(statement);
    return out;
}

constexpr const char* kCatalogueColumns =
    "id, name, circuit, venue_id, west, south, east, north, centerline_length_m, "
    "published_length_m, median_width_m, principal_axis_deg, has_centerline, closed, combo, "
    "quality, outline_points, gate_source, gate_centre_lat, gate_centre_lon, gate_left_lat, "
    "gate_left_lon, gate_right_lat, gate_right_lon, gate_offset_cm, gate_width_m";

} // namespace

const char* to_string(Quality quality)
{
    switch (quality)
    {
        case Quality::Unknown:
            return "unknown";
        case Quality::Ok:
            return "ok";
        case Quality::SeamNotFound:
            return "seam-not-found";
        case Quality::MultipleLoops:
            return "multiple-loops";
        case Quality::WidthOutOfRange:
            return "width-out-of-range";
        case Quality::LengthMismatch:
            return "length-mismatch";
        case Quality::SourceLengthImplausible:
            return "source-length-implausible";
        case Quality::Degenerate:
            return "degenerate";
    }
    return "unknown";
}

const char* to_string(GateSource source)
{
    switch (source)
    {
        case GateSource::None:
            return "none";
        case GateSource::DataDrop:
            return "data-drop";
        case GateSource::Derived:
            return "derived";
        case GateSource::Manual:
            return "manual";
    }
    return "none";
}

const char* to_string(GeometryKind kind)
{
    switch (kind)
    {
        case GeometryKind::OuterRing:
            return "outer-ring";
        case GeometryKind::InnerRing:
            return "inner-ring";
        case GeometryKind::Centerline:
            return "centerline";
        case GeometryKind::CenterlineDistanceCm:
            return "centerline-distance-cm";
        case GeometryKind::HalfWidthCm:
            return "half-width-cm";
    }
    return "unknown";
}

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::NotFound:
            return "not found";
        case Error::Kind::NotReadable:
            return "not readable";
        case Error::Kind::NoCatalogue:
            return "no track catalogue";
        case Error::Kind::BuildMismatch:
            return "build id mismatch";
        case Error::Kind::InvalidArgument:
            return "invalid argument";
        case Error::Kind::Query:
            return "query failed";
    }
    return "unknown";
}

std::string to_string(const Error& error)
{
    std::string out = to_string(error.kind);
    if (!error.message.empty())
    {
        out += ": ";
        out += error.message;
    }
    if (error.code != 0)
    {
        out += " (sqlite ";
        out += std::to_string(error.code);
        out += ")";
    }
    return out;
}

// ============================================================================

std::size_t Blob::valueCount() const
{
    const std::size_t width = valueWidth(kind);
    if (width == 0 || data.size() < kBlobHeaderBytes)
    {
        return 0;
    }
    return (data.size() - kBlobHeaderBytes) / width;
}

std::vector<std::int32_t> Blob::asCoords() const
{
    if (kind != GeometryKind::OuterRing && kind != GeometryKind::InnerRing &&
        kind != GeometryKind::Centerline)
    {
        return {};
    }
    return decode<std::int32_t>(data);
}

std::vector<std::uint32_t> Blob::asUint32() const
{
    if (kind != GeometryKind::CenterlineDistanceCm)
    {
        return {};
    }
    return decode<std::uint32_t>(data);
}

std::vector<std::uint16_t> Blob::asUint16() const
{
    if (kind != GeometryKind::HalfWidthCm)
    {
        return {};
    }
    return decode<std::uint16_t>(data);
}

// ============================================================================

struct Store::Impl
{
    sqlite3* db { nullptr };
    std::filesystem::path path;
    std::string buildId;
    std::vector<TrackRecord> tracks;

    ~Impl()
    {
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
    }
};

Store::Store() : mImpl(std::make_unique<Impl>()) {}
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

Result<Store> Store::open(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return fail(Error::Kind::NotFound, path.string());
    }

    Store store;
    const int rc =
        sqlite3_open_v2(path.string().c_str(), &store.mImpl->db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        return fail(Error::Kind::NotReadable, path.string(), rc);
    }
    store.mImpl->path = path;

    if (!hasTable(store.mImpl->db, "track_catalog") || !hasTable(store.mImpl->db, "track_meta"))
    {
        return fail(Error::Kind::NoCatalogue, path.string());
    }

    const auto ours = readMeta(store.mImpl->db, "track_meta", kBuildIdKey);
    const auto theirs = readMeta(store.mImpl->db, "metadata", kBuildIdKey);
    if (!ours.has_value())
    {
        return fail(Error::Kind::NoCatalogue, "track_meta has no build_id");
    }
    // The check the one-artifact claim rests on. If the tiles and the catalogue
    // came from different runs, everything still opens and everything still
    // draws -- and a lap distance is measured against the wrong centreline.
    if (theirs.has_value() && *theirs != *ours)
    {
        return fail(Error::Kind::BuildMismatch,
                    "tiles say '" + *theirs + "', catalogue says '" + *ours + "'");
    }
    store.mImpl->buildId = *ours;

    const std::string sql =
        std::string("SELECT ") + kCatalogueColumns + " FROM track_catalog ORDER BY id";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(store.mImpl->db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(store.mImpl->db));
    }

    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        TrackRecord record;
        int c = 0;
        record.id = textOr(statement, c++);
        record.name = textOr(statement, c++);
        record.circuit = textOr(statement, c++);
        record.venueId = textOr(statement, c++);
        record.west = sqlite3_column_double(statement, c++);
        record.south = sqlite3_column_double(statement, c++);
        record.east = sqlite3_column_double(statement, c++);
        record.north = sqlite3_column_double(statement, c++);
        record.centerlineLengthM = sqlite3_column_double(statement, c++);
        record.publishedLengthM = sqlite3_column_double(statement, c++);
        record.medianWidthM = sqlite3_column_double(statement, c++);
        record.principalAxisDeg = sqlite3_column_double(statement, c++);
        record.hasCenterline = sqlite3_column_int(statement, c++) != 0;
        record.closed = sqlite3_column_int(statement, c++) != 0;
        record.combo = sqlite3_column_int(statement, c++) != 0;
        record.quality = static_cast<Quality>(sqlite3_column_int(statement, c++));
        record.outlinePoints = static_cast<std::uint32_t>(sqlite3_column_int64(statement, c++));
        record.gate.source = static_cast<GateSource>(sqlite3_column_int(statement, c++));
        record.gate.centreLatE7 = sqlite3_column_int(statement, c++);
        record.gate.centreLonE7 = sqlite3_column_int(statement, c++);
        record.gate.leftLatE7 = sqlite3_column_int(statement, c++);
        record.gate.leftLonE7 = sqlite3_column_int(statement, c++);
        record.gate.rightLatE7 = sqlite3_column_int(statement, c++);
        record.gate.rightLonE7 = sqlite3_column_int(statement, c++);
        record.gate.centerlineOffsetCm =
            static_cast<std::uint32_t>(sqlite3_column_int64(statement, c++));
        record.gate.widthM = sqlite3_column_double(statement, c++);
        store.mImpl->tracks.push_back(std::move(record));
    }
    sqlite3_finalize(statement);

    return store;
}

const std::string& Store::buildId() const
{
    return mImpl->buildId;
}

const std::filesystem::path& Store::path() const
{
    return mImpl->path;
}

const std::vector<TrackRecord>& Store::tracks() const
{
    return mImpl->tracks;
}

const TrackRecord* Store::find(std::string_view id) const
{
    // Sorted by id at load, so this is a binary search rather than a scan.
    const auto found = std::lower_bound(
        mImpl->tracks.begin(), mImpl->tracks.end(), id,
        [](const TrackRecord& record, std::string_view key) { return record.id < key; });
    if (found == mImpl->tracks.end() || found->id != id)
    {
        return nullptr;
    }
    return &*found;
}

Result<std::optional<Blob>> Store::geometry(std::string_view id, GeometryKind kind) const
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(mImpl->db, "SELECT data FROM track_geometry WHERE id=? AND kind=?", -1,
                           &statement, nullptr) != SQLITE_OK)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(mImpl->db));
    }
    sqlite3_bind_text(statement, 1, id.data(), static_cast<int>(id.size()), sqliteStatic());
    sqlite3_bind_int(statement, 2, static_cast<int>(kind));

    std::optional<Blob> out;
    if (sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, 0));
        const int size = sqlite3_column_bytes(statement, 0);
        if (bytes != nullptr && size >= static_cast<int>(kBlobHeaderBytes))
        {
            if (bytes[0] == kBlobVersion && bytes[1] == static_cast<std::uint8_t>(kind))
            {
                Blob blob;
                blob.kind = kind;
                blob.data.assign(bytes, bytes + size);
                out = std::move(blob);
            }
            else
            {
                sqlite3_finalize(statement);
                return fail(Error::Kind::Query, "geometry blob is not the version or kind it "
                                                "was asked for");
            }
        }
    }
    sqlite3_finalize(statement);
    return out;
}

// ============================================================================

struct Writer::Impl
{
    sqlite3* db { nullptr };
    bool finished { false };

    ~Impl()
    {
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
    }
};

Writer::Writer() : mImpl(std::make_unique<Impl>()) {}
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;
Writer::~Writer() = default;

Result<Writer> Writer::append(const std::filesystem::path& path, const std::string& buildId)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return fail(Error::Kind::NotFound, path.string());
    }

    Writer writer;
    const int rc =
        sqlite3_open_v2(path.string().c_str(), &writer.mImpl->db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK)
    {
        return fail(Error::Kind::NotReadable, path.string(), rc);
    }

    if (auto ok = exec(writer.mImpl->db, R"(
            PRAGMA journal_mode=OFF;
            PRAGMA synchronous=OFF;
            DROP TABLE IF EXISTS track_catalog;
            DROP TABLE IF EXISTS track_geometry;
            DROP TABLE IF EXISTS track_meta;
            CREATE TABLE track_meta (name text PRIMARY KEY, value text);
            CREATE TABLE track_catalog (
                id text PRIMARY KEY, name text, circuit text, venue_id text,
                west real, south real, east real, north real,
                centerline_length_m real, published_length_m real, median_width_m real,
                principal_axis_deg real, has_centerline integer, closed integer, combo integer,
                quality integer, outline_points integer,
                gate_source integer, gate_centre_lat integer, gate_centre_lon integer,
                gate_left_lat integer, gate_left_lon integer,
                gate_right_lat integer, gate_right_lon integer,
                gate_offset_cm integer, gate_width_m real);
            CREATE TABLE track_geometry (
                id text, kind integer, data blob, PRIMARY KEY (id, kind));
            BEGIN;
        )");
        !ok)
    {
        return std::unexpected(ok.error());
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(writer.mImpl->db,
                           "INSERT OR REPLACE INTO track_meta (name, value) VALUES (?, ?)", -1,
                           &statement, nullptr) != SQLITE_OK)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(writer.mImpl->db));
    }
    const std::pair<const char*, std::string> rows[] = {
        { kBuildIdKey, buildId },
        { kSchemaVersionKey, kSchemaVersion },
    };
    for (const auto& [key, value] : rows)
    {
        sqlite3_reset(statement);
        sqlite3_bind_text(statement, 1, key, -1, sqliteStatic());
        sqlite3_bind_text(statement, 2, value.c_str(), -1, sqliteTransient());
        if (sqlite3_step(statement) != SQLITE_DONE)
        {
            sqlite3_finalize(statement);
            return fail(Error::Kind::Query, sqlite3_errmsg(writer.mImpl->db));
        }
    }
    sqlite3_finalize(statement);

    // The SAME build id into the mbtiles metadata table, which mbtiles::Writer
    // has already populated and closed. This is the other half of the pair
    // Store::open() checks.
    if (sqlite3_prepare_v2(writer.mImpl->db,
                           "INSERT OR REPLACE INTO metadata (name, value) VALUES (?, ?)", -1,
                           &statement, nullptr) != SQLITE_OK)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(writer.mImpl->db));
    }
    sqlite3_bind_text(statement, 1, kBuildIdKey, -1, sqliteStatic());
    sqlite3_bind_text(statement, 2, buildId.c_str(), -1, sqliteTransient());
    const int step = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(writer.mImpl->db));
    }

    return writer;
}

Result<void> Writer::put(const TrackRecord& record)
{
    const std::string sql = std::string("INSERT OR REPLACE INTO track_catalog (") +
                            kCatalogueColumns +
                            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(mImpl->db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(mImpl->db));
    }

    int c = 1;
    sqlite3_bind_text(statement, c++, record.id.c_str(), -1, sqliteTransient());
    sqlite3_bind_text(statement, c++, record.name.c_str(), -1, sqliteTransient());
    sqlite3_bind_text(statement, c++, record.circuit.c_str(), -1, sqliteTransient());
    sqlite3_bind_text(statement, c++, record.venueId.c_str(), -1, sqliteTransient());
    sqlite3_bind_double(statement, c++, record.west);
    sqlite3_bind_double(statement, c++, record.south);
    sqlite3_bind_double(statement, c++, record.east);
    sqlite3_bind_double(statement, c++, record.north);
    sqlite3_bind_double(statement, c++, record.centerlineLengthM);
    sqlite3_bind_double(statement, c++, record.publishedLengthM);
    sqlite3_bind_double(statement, c++, record.medianWidthM);
    sqlite3_bind_double(statement, c++, record.principalAxisDeg);
    sqlite3_bind_int(statement, c++, record.hasCenterline ? 1 : 0);
    sqlite3_bind_int(statement, c++, record.closed ? 1 : 0);
    sqlite3_bind_int(statement, c++, record.combo ? 1 : 0);
    sqlite3_bind_int(statement, c++, static_cast<int>(record.quality));
    sqlite3_bind_int64(statement, c++, record.outlinePoints);
    sqlite3_bind_int(statement, c++, static_cast<int>(record.gate.source));
    sqlite3_bind_int(statement, c++, record.gate.centreLatE7);
    sqlite3_bind_int(statement, c++, record.gate.centreLonE7);
    sqlite3_bind_int(statement, c++, record.gate.leftLatE7);
    sqlite3_bind_int(statement, c++, record.gate.leftLonE7);
    sqlite3_bind_int(statement, c++, record.gate.rightLatE7);
    sqlite3_bind_int(statement, c++, record.gate.rightLonE7);
    sqlite3_bind_int64(statement, c++, record.gate.centerlineOffsetCm);
    sqlite3_bind_double(statement, c++, record.gate.widthM);

    const int step = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(mImpl->db));
    }
    return {};
}

namespace
{

Result<void> putBlob(sqlite3* db, std::string_view id, GeometryKind kind,
                     const std::vector<std::uint8_t>& bytes)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR REPLACE INTO track_geometry (id, kind, data) "
                           "VALUES (?, ?, ?)",
                           -1, &statement, nullptr) != SQLITE_OK)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(db));
    }
    sqlite3_bind_text(statement, 1, id.data(), static_cast<int>(id.size()), sqliteTransient());
    sqlite3_bind_int(statement, 2, static_cast<int>(kind));
    sqlite3_bind_blob(statement, 3, bytes.data(), static_cast<int>(bytes.size()),
                      sqliteTransient());
    const int step = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE)
    {
        return fail(Error::Kind::Query, sqlite3_errmsg(db));
    }
    return {};
}

} // namespace

Result<void> Writer::putGeometry(std::string_view id, GeometryKind kind,
                                 std::span<const std::int32_t> coords)
{
    if (valueWidth(kind) != sizeof(std::int32_t))
    {
        return fail(Error::Kind::InvalidArgument, "kind does not hold coordinates");
    }
    return putBlob(mImpl->db, id, kind, encode(kind, coords));
}

Result<void> Writer::putGeometry(std::string_view id, GeometryKind kind,
                                 std::span<const std::uint32_t> values)
{
    if (kind != GeometryKind::CenterlineDistanceCm)
    {
        return fail(Error::Kind::InvalidArgument, "kind does not hold 32-bit values");
    }
    return putBlob(mImpl->db, id, kind, encode(kind, values));
}

Result<void> Writer::putGeometry(std::string_view id, GeometryKind kind,
                                 std::span<const std::uint16_t> values)
{
    if (kind != GeometryKind::HalfWidthCm)
    {
        return fail(Error::Kind::InvalidArgument, "kind does not hold 16-bit values");
    }
    return putBlob(mImpl->db, id, kind, encode(kind, values));
}

Result<void> Writer::finish()
{
    if (mImpl->finished)
    {
        return {};
    }
    mImpl->finished = true;
    // The index AFTER the rows, as mbtiles::Writer does: maintaining it across
    // a few thousand inserts costs several times what building it once does.
    return exec(mImpl->db, "COMMIT; CREATE INDEX track_catalog_venue "
                           "ON track_catalog (venue_id);");
}

} // namespace track_store
