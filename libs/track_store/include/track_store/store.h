// SPDX-License-Identifier: GPL-3.0-or-later
//
// The track catalogue, which lives INSIDE the .mbtiles.
//
// An mbtiles archive is a SQLite database, and mbtiles::Archive reads exactly
// two tables out of it -- `tiles` and `metadata` -- with SQLITE_OPEN_READONLY
// and no CREATE. Extra tables are therefore invisible to it, and the tile path
// is completely unaffected by their being there.
//
// That is what makes ONE ARTIFACT possible. Tracks need two different things
// from the same data: tiles, for drawing them on a map, and full-resolution
// geometry with a centreline and a start/finish gate, for everything that comes
// later. A sidecar file beside the archive would have been the obvious way to
// carry the second, and it is the wrong one: two files that can disagree about
// which build they came from, with no symptom until something computes a lap
// distance against a centreline from a different ingest run. One file cannot
// disagree with itself.
//
// The guard that makes that a checkable claim rather than a hope is `build_id`,
// written into BOTH the mbtiles `metadata` table and this library's own
// `track_meta`, and refused at open when the two differ. That is the state a
// half-finished or half-copied build leaves behind.
//
// NO ZENOH, NO QT. nodes/map_server is what puts these answers on the bus, and
// the dashboard never links this at all -- it asks the server. Keeping SQLite
// out of the GUI is not tidiness: Qt reaches SQLite through Qt6::Sql with its
// own copy, and the two must never meet. Same rule as libs/mbtiles.
#ifndef TRACK_STORE_STORE_H
#define TRACK_STORE_STORE_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "track_store/types.h"

namespace track_store
{

struct Error
{
    enum class Kind
    {
        NotFound,
        NotReadable,
        // Opened, but it carries no track tables. Distinct from NotReadable
        // because it is the normal state of an ordinary basemap archive, and a
        // server configured with one wants to say so rather than claim the file
        // is broken.
        NoCatalogue,
        // `build_id` disagrees between the two tables. A half-written or
        // half-copied build.
        BuildMismatch,
        InvalidArgument,
        Query,
    };

    Kind kind { Kind::Query };
    std::string message;
    int code { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

// One geometry array, as stored.
//
// Raw little-endian fixed-width values behind a four-byte header, not capnp:
// capnp is the WIRE, and a format written and read by one build has no
// schema-evolution story to buy. The version byte is there so a reader can
// refuse a blob it does not understand rather than decode it as garbage.
struct Blob
{
    GeometryKind kind { GeometryKind::OuterRing };
    // Interpreted according to `kind`: int32 for the rings and the centreline,
    // uint32 for distances, uint16 for half widths.
    std::vector<std::uint8_t> data;

    std::size_t valueCount() const;
    // Interleaved lat/lon pairs, for the three coordinate kinds. Empty for the
    // others.
    std::vector<std::int32_t> asCoords() const;
    std::vector<std::uint32_t> asUint32() const;
    std::vector<std::uint16_t> asUint16() const;
};

// Reading. The whole catalogue is loaded at open and the database is kept only
// for the geometry blobs.
//
// Loaded rather than queried per request because it is small -- 994 tracks is
// under a megabyte of records -- and because the alternative is a connection
// pool answering point lookups on several zenoh RX threads. The blobs stay on
// disk: the outlines are 49 MB of source and one track's worth is all anybody
// asks for at a time.
class Store
{
  public:
    // Read-only, and FAILS if the file is absent. SQLite's default would make
    // an empty database and then serve nothing, which looks exactly like a
    // correctly configured server with no tracks in range.
    static Result<Store> open(const std::filesystem::path& path);

    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store();

    // Identifies the ARTIFACT, and travels with every reply built from it. A
    // consumer holding geometry from one build and a catalogue from another
    // computes against the wrong centreline and renders perfectly.
    const std::string& buildId() const;
    const std::filesystem::path& path() const;

    const std::vector<TrackRecord>& tracks() const;
    const TrackRecord* find(std::string_view id) const;

    // Absent -- rather than an error -- when the track has no geometry of that
    // kind. A track that failed the QA gate has an outline and no centreline,
    // and that is a normal answer.
    Result<std::optional<Blob>> geometry(std::string_view id, GeometryKind kind) const;

  private:
    Store();
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// Writing, into an archive that already exists.
//
// APPEND, not create. The tiles are written first by mbtiles::Writer, and this
// opens the finished file a second time to add the catalogue beside them.
//
// The ordering is load-bearing and easy to get wrong: mbtiles::Writer::finish()
// COMMITS BUT DOES NOT CLOSE -- only its destructor does -- so this must be
// called after that object has gone out of scope. Two write handles on one
// SQLite file is how a half-written archive happens.
class Writer
{
  public:
    static Result<Writer> append(const std::filesystem::path& path, const std::string& buildId);

    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();

    Result<void> put(const TrackRecord& record);
    Result<void> putGeometry(std::string_view id, GeometryKind kind,
                             std::span<const std::int32_t> coords);
    Result<void> putGeometry(std::string_view id, GeometryKind kind,
                             std::span<const std::uint32_t> values);
    Result<void> putGeometry(std::string_view id, GeometryKind kind,
                             std::span<const std::uint16_t> values);

    // Commit and index. As in mbtiles::Writer, the index is built after the
    // rows rather than maintained across them.
    Result<void> finish();

  private:
    Writer();
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace track_store

#endif // TRACK_STORE_STORE_H
