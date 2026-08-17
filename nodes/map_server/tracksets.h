// SPDX-License-Identifier: GPL-3.0-or-later
//
// The race-track catalogues this node has open, by the name clients ask for.
//
// Same shape as TilesetRegistry and for the same reason: a trackset whose file
// failed to open is KEPT, with its error, because "you asked for something that
// is not configured" and "it is configured and I cannot read it" are different
// answers, and collapsing them makes a permissions problem look like a typo.
//
// One difference worth stating. A trackset normally points at the SAME FILE as
// a tileset -- the catalogue lives in extra tables inside the .mbtiles -- so the
// server holds two read-only SQLite connections to it, one through
// mbtiles::Archive and one through track_store::Store. That is fine, and it is
// not a duplication to tidy away: the two read different tables to answer
// different questions, and either can be present without the other. An ordinary
// basemap has no catalogue, and a catalogue whose tiles have been rebuilt
// separately is refused at open by the build-id check.
#ifndef MAP_SERVER_TRACKSETS_H
#define MAP_SERVER_TRACKSETS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "track_store/store.h"

#include "node_config.h"

namespace map_server
{

struct Trackset
{
    std::string name;
    std::string path;

    // Empty when the catalogue could not be opened; `error` says why. The
    // commonest reason by far is that the file is an ordinary basemap with no
    // track tables at all, which is reported as such rather than as breakage.
    std::unique_ptr<track_store::Store> store;
    std::string error;

    // Counters, written from zenoh query threads.
    std::atomic<std::uint64_t> catalogQueries { 0 };
    std::atomic<std::uint64_t> detailQueries { 0 };
    std::atomic<std::uint64_t> missing { 0 };
};

class TracksetRegistry
{
  public:
    explicit TracksetRegistry(const std::vector<TracksetConfig>& configured);

    // Null when nothing by that name is configured. An EMPTY name asks for the
    // first one, because there is only ever one track layer in practice and
    // making every client name it would be ceremony.
    Trackset* find(std::string_view name);
    const Trackset* find(std::string_view name) const;

    const std::vector<std::unique_ptr<Trackset>>& all() const { return mTracksets; }

    std::size_t openCount() const;

  private:
    std::vector<std::unique_ptr<Trackset>> mTracksets;
};

// Douglas-Peucker over interleaved lat/lon in 1e-7 degrees, in metres.
//
// Server side because the caller knows its zoom and the server knows the
// geometry: Milford Road Course is 55 000 points, which is 440 kB on the wire
// to draw a shape a few hundred pixels across. A tolerance of zero means "send
// everything", and is CLAMPED to a floor rather than honoured -- a client that
// did not think about it should still get an answer it can draw.
//
// Free and pure so it can be tested without a node, a bus or a file.
std::vector<std::int32_t> simplifyCoords(const std::vector<std::int32_t>& lonLatE7,
                                         double toleranceM, bool closed);

} // namespace map_server

#endif // MAP_SERVER_TRACKSETS_H
