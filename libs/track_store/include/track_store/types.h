// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a track catalogue holds, with no SQLite in sight.
//
// Split from store.h so a consumer can name a TrackRecord without linking the
// storage: nodes/map_server puts these on the wire, and the translation from
// record to capnp has no business including a database header. Same split as
// mbtiles::Metadata against mbtiles::Archive.
#ifndef TRACK_STORE_TYPES_H
#define TRACK_STORE_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace track_store
{

// Why a track has no usable centreline. Mirrors map_build::track::Quality --
// the ingest's judgement, carried through to whatever asks.
//
// Values are PERSISTED as integers in the catalogue, so they may be appended to
// and never reordered.
enum class Quality : std::uint8_t
{
    Unknown = 0,
    Ok = 1,
    SeamNotFound = 2,
    MultipleLoops = 3,
    WidthOutOfRange = 4,
    LengthMismatch = 5,
    SourceLengthImplausible = 6,
    Degenerate = 7,
};

const char* to_string(Quality quality);

// Where a start/finish gate came from. Persisted; append only.
enum class GateSource : std::uint8_t
{
    None = 0,
    // The source GeoJSON's own Start / Finish point.
    DataDrop = 1,
    // Worked out from the geometry because the source had none.
    Derived = 2,
    // Placed by hand.
    Manual = 3,
};

const char* to_string(GateSource source);

// The start/finish line.
//
// A CENTRE and two ends, not a point. A gate is a line the vehicle crosses, and
// a crossing test against a point alone is a distance threshold -- at 250 km/h
// a 10 Hz fix moves 7 m between samples, so a threshold either fires late or
// fires twice. Nothing in this milestone crosses it; it is stored so that
// whatever eventually does is not blocked on a re-ingest.
struct Gate
{
    GateSource source { GateSource::None };
    std::int32_t centreLatE7 { 0 };
    std::int32_t centreLonE7 { 0 };
    std::int32_t leftLatE7 { 0 };
    std::int32_t leftLonE7 { 0 };
    std::int32_t rightLatE7 { 0 };
    std::int32_t rightLonE7 { 0 };
    // How far the centreline's first sample sits from the point the source
    // gave. Under one sample spacing by construction.
    std::uint32_t centerlineOffsetCm { 0 };
    double widthM { 0.0 };

    bool present() const { return source != GateSource::None; }
};

struct TrackRecord
{
    // The source file stem. STABLE ACROSS REBUILDS, which is why it is the
    // filename and not an index, a geometry hash, or the display name -- two
    // files are called nothing at all and several share a `circuit`.
    std::string id;
    std::string name;
    std::string circuit;

    // The largest layout at the same place.
    //
    // NOT STABLE ACROSS REBUILDS. It names whichever member happened to be
    // largest among the files present, so a client may use it within one reply
    // and must never persist it. Paired with buildId on the wire for exactly
    // that reason.
    std::string venueId;

    double west { 0.0 };
    double south { 0.0 };
    double east { 0.0 };
    double north { 0.0 };

    double centerlineLengthM { 0.0 };
    // From the source's own Start / Finish point. ZERO MEANS THE FILE DID NOT
    // SAY -- not that the lap is zero long, and not that the two agree.
    double publishedLengthM { 0.0 };
    double medianWidthM { 0.0 };
    double principalAxisDeg { 0.0 };

    bool hasCenterline { false };
    bool closed { false };
    bool combo { false };
    Quality quality { Quality::Unknown };
    std::uint32_t outlinePoints { 0 };

    Gate gate;
};

// Which array a geometry blob holds. Persisted; append only.
enum class GeometryKind : std::uint8_t
{
    // Interleaved lat/lon in 1e-7 degrees, first point not repeated.
    OuterRing = 0,
    InnerRing = 1,
    Centerline = 2,
    // Parallel to Centerline, one per point: distance from the gate, and half
    // the local track width.
    CenterlineDistanceCm = 3,
    HalfWidthCm = 4,
};

const char* to_string(GeometryKind kind);

} // namespace track_store

#endif // TRACK_STORE_TYPES_H
