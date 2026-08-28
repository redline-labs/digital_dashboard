// SPDX-License-Identifier: GPL-3.0-or-later
//
// Label candidates: everything camera-free about a tile's labels, extracted
// once at decode time on the worker that decoded the tile.
//
// A header of its own, below map/labels.h, because the tile CACHE stores
// these -- and the cache is deliberately testable with QtCore alone, while
// the label pass proper paints and drags in QtGui.
#ifndef MAP_LABEL_CANDIDATES_H
#define MAP_LABEL_CANDIDATES_H

#include <cstdint>
#include <vector>

#include <QString>

#include "mvt/tile.h"

namespace map_widget
{

// Which label pass a candidate belongs to, deciding its per-frame gate:
// places and circuit names always draw; road and water names each have a
// toggle and a zoom floor of their own.
enum class LabelKind : std::uint8_t
{
    Place,
    Track,
    Road,
    Water,
};

// One label candidate -- everything about a label that does not depend on the
// camera. What stays per-frame is placement: projecting the anchor, gating by
// zoom, and viewport-global collision, which cannot be baked per tile (see
// map/labels.h).
//
// This is also what lets the cache drop the decoded mvt::Tile entirely: the
// label pass was its only consumer, and a dense z14 tile's decoded features
// weigh hundreds of kilobytes against the few kilobytes of candidates here.
struct LabelCandidate
{
    QString text;
    // Tile-local anchor in [0,1] across the tile -- the same camera-free
    // domain the GPU's vertices use, and for the same reason.
    double x { 0.0 };
    double y { 0.0 };
    // Arc length of the feature's longest run, in the same [0,1] domain.
    // Zero for point labels. Multiplying by the tile's on-screen size gives
    // the on-screen run length, because the tile-to-screen transform is a
    // similarity and changes no ratio.
    double spanLocal { 0.0 };
    int priority { 0 };
    std::uint32_t magnitude { 0 };
    LabelKind kind { LabelKind::Place };
    bool oneLabelPerName { false };

    // The run itself, as a range into LabelSet::path. Empty for a point label.
    //
    // A road's name is drawn ALONG the road, so the label pass needs the
    // shape and not just a point on it. Held as a range into one flat arena
    // rather than a vector per candidate: a tile's labels are built once and
    // read many times, and this is one allocation instead of hundreds.
    std::uint32_t pathBegin { 0 };
    std::uint32_t pathCount { 0 };
};

// One point of a run, tile-local in [0,1].
//
// Float, not double: [0,1] in float resolves to about 6e-8, which is three
// orders of magnitude finer than a z14 tile's own 1/4096 quantum, so this
// discards nothing the archive ever carried and halves the arena.
struct LocalPoint
{
    float x { 0.0F };
    float y { 0.0F };
};

// Everything label-worthy in one decoded tile.
//
// A struct rather than a bare vector because line labels carry geometry: the
// candidates and the flat arena their runs point into travel together and
// have the same lifetime.
struct LabelSet
{
    std::vector<LabelCandidate> labels;
    std::vector<LocalPoint> path;

    bool empty() const { return labels.empty(); }
    std::size_t size() const { return labels.size(); }
};

// Runs on the decode worker, off the GUI thread; QString is a value type and
// crosses safely.
LabelSet extractLabels(const mvt::Tile& tile);

} // namespace map_widget

#endif // MAP_LABEL_CANDIDATES_H
