// SPDX-License-Identifier: GPL-3.0-or-later
//
// One directory of track GeoJSON, turned into everything the archive needs.
//
// Split out of the verb so the pipeline can be run without a CLI or an output
// file: point it at a directory and get back what it made of every file,
// including the ones it could not use. The verb on top of it does argument
// parsing, tiling and reporting and nothing else.
#ifndef MAP_BUILD_TRACKS_H
#define MAP_BUILD_TRACKS_H

#include <filesystem>
#include <string>
#include <vector>

#include "map_build/track_geometry.h"
#include "map_build/track_source.h"

namespace map_build
{

// [west, south, east, north] in degrees -- the order mbtiles metadata and
// MapTileset.bounds already use, so nothing downstream has to reorder it.
struct TrackBounds
{
    double west { 0.0 };
    double south { 0.0 };
    double east { 0.0 };
    double north { 0.0 };
};

struct IngestedTrack
{
    std::string id;
    std::string name;
    std::string circuit;
    // The largest layout at the same place. Several layouts are the same
    // tarmac driven differently and they overlap on the ground.
    //
    // NOT STABLE ACROSS REBUILDS: it names whichever member happened to be the
    // largest in the set of files that were present. Nothing may persist it.
    std::string venueId;

    bool combo { false };
    // Whether the DERIVED CENTRELINE joins up -- a circuit rather than a
    // point-to-point course. Not the source's `closed` property and not the
    // feature type: every outline in this database is a closed Polygon, so
    // neither of those can tell the two apart. See track_geometry.h.
    bool closed { false };

    // As read: the whole outline, both loops, one ring.
    track::Ring outline;
    TrackBounds bounds;
    double principalAxisDeg { 0.0 };

    // The split, the centreline, and why there is or is not one.
    track::Derived derived;

    track::GatePointStatus gatePoint { track::GatePointStatus::Absent };
    track::GateResult gateResult { track::GateResult::NoPoint };
    track::Gate gate;
};

// A file that never became a track. Carried alongside rather than logged and
// forgotten: this is the set most likely to be missed, and it belongs in the
// report next to what succeeded.
struct SkippedFile
{
    std::string id;
    track::LoadStatus status { track::LoadStatus::Ok };
    std::string error;
};

struct IngestOptions
{
    track::DeriveOptions derive;
    double venueExpandM { 300.0 };
};

std::vector<IngestedTrack> ingestTracks(const std::filesystem::path& directory,
                                        const IngestOptions& options,
                                        std::vector<SkippedFile>& skipped);

} // namespace map_build

#endif // MAP_BUILD_TRACKS_H
