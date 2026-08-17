// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading one track's GeoJSON.
//
// The vocabulary is narrow and worth stating, because "GeoJSON" suggests
// something far more general than what these files are. Each holds a
// FeatureCollection of at most two features:
//
//   1. the OUTLINE -- a Polygon (978 files) or a LineString (16, the layouts
//      that do not close). Its `properties` carry `name`, `closed` and
//      `degenerate`, and nothing else worth keeping;
//   2. a POINT named "Start / Finish", added in the 2026-08 data drop, carrying
//      `circuit` (the canonical name), `length_m` (the PUBLISHED lap length),
//      `gatewidth_m` and `combo`.
//
// `length_m` is the reason this reader exists rather than a one-line json call.
// It is the only independent measurement of a track anywhere in the pipeline,
// and the derivation's QA gate is built on it -- so how it is read, and what
// happens when it is missing, is a correctness question rather than a parsing
// convenience.
//
// Nothing here is a general GeoJSON parser. A file that does not match the
// shape above is REPORTED AND SKIPPED, never coerced: an ingest that quietly
// half-understands a file produces a track that is subtly in the wrong place,
// which nobody discovers until they drive it.
#ifndef MAP_BUILD_TRACK_SOURCE_H
#define MAP_BUILD_TRACK_SOURCE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "map_build/track_geometry.h"
#include "osm/entity.h"

namespace map_build::track
{

// Why a file was skipped. One value per thing that can be wrong with it, so the
// report can say which rather than counting failures.
enum class LoadStatus : std::uint8_t
{
    Ok,
    Unreadable,
    NotJson,
    NotFeatureCollection,
    // No Polygon or LineString feature.
    NoOutline,
    // More than one, and no rule says which is the track.
    SeveralOutlines,
    // Fewer than three points, or coordinates that are not pairs of numbers.
    BadGeometry,
};

const char* to_string(LoadStatus status);

// What the file said about its start/finish point, kept apart from whether a
// gate could actually be PLACED -- that is geometry, and it happens later.
enum class GatePointStatus : std::uint8_t
{
    Present,
    // No Point feature. Two files are like this.
    Absent,
    // Several. Guessing which is meant is worse than declining.
    Several,
};

const char* to_string(GatePointStatus status);

struct SourceFile
{
    // The file stem. This is the identity that reaches the wire and it must
    // stay STABLE across rebuilds, which is why it is the filename and not an
    // index, a hash of the geometry, or the track's display name -- two files
    // are called nothing at all and several share a `circuit`.
    std::string id;
    std::string name;
    std::string circuit;

    // Interleaved lat/lon, first point not repeated.
    Ring outline;
    bool closed { false };
    bool degenerate { false };
    bool combo { false };

    // From the Start / Finish point. Zero means the file did not say, which is
    // NOT the same as zero length -- see DeriveOptions and the QA gate.
    double publishedLengthM { 0.0 };
    double gateWidthM { 0.0 };

    GatePointStatus gatePoint { GatePointStatus::Absent };
    osm::Coord gateLat { 0 };
    osm::Coord gateLon { 0 };
};

struct LoadResult
{
    LoadStatus status { LoadStatus::Ok };
    // Populated when status is Ok.
    SourceFile file;
    // What went wrong, in enough detail to fix the data.
    std::string error;
};

LoadResult loadSourceFile(const std::filesystem::path& path);

// Every *.geojson in `directory`, in sorted order.
//
// Sorted because the build id is a hash over what went in, and a directory
// iteration order that varies between filesystems would make two builds of
// identical data claim to be different.
std::vector<std::filesystem::path> listSourceFiles(const std::filesystem::path& directory);

} // namespace map_build::track

#endif // MAP_BUILD_TRACK_SOURCE_H
