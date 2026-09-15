#ifndef SCOPE_MAP_TRACK_BUILDER_H_
#define SCOPE_MAP_TRACK_BUILDER_H_

// Turning two signals into a line on a map, and finding a point on it.
//
// A FREE HEADER WITH NO WIDGET BEHIND IT, the way scope/state_names.h and
// map_render/tile_cache.h are, because every way this can be wrong DRAWS
// SOMETHING rather than raising anything:
//
//   * pair latitude and longitude by INDEX instead of by timestamp and the
//     whole track skews the moment one binding is added after the other, or one
//     sample is dropped. The line is still a line, over the wrong roads.
//   * interpolate the colour signal and a corner is painted a speed nothing
//     ever published.
//   * get the pixel thinning wrong and either the path costs a thousand
//     segments per pixel or it visibly corners.
//   * get the hit test wrong and clicking the track seeks somewhere else, which
//     reads as a mis-aimed mouse.
//
// None of it needs a window, a GPU or a bus, so all of it is a unit test.
#include "map_render/projection.h"
#include "scope/sample_ring.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace scope::track
{

// One position the vehicle was at, at one instant.
struct Point
{
    // Source-clock seconds -- the same axis the plots and the shared cursor use,
    // which is what lets a click here become a time.
    double t { 0.0 };

    // Mercator world coordinates rather than degrees, so a pan does not
    // re-project the whole track every frame. Measured at 3 us/frame either
    // way, so this is about not doing arithmetic twice rather than about speed.
    map_render::WorldPoint world;

    // The value driving the colour ramp, held from the newest sample at or
    // before `t`. False when nothing is bound or nothing had arrived yet.
    double color { 0.0 };
    bool has_color { false };
};

// What pairing actually managed, as opposed to what was asked for.
//
// THE UNPAIRED COUNTS ARE THE POINT. "The map is empty" has several causes that
// look identical, and "3000 latitudes arrived and none of them found a
// longitude" is the one that says the two signals are not on the same topic.
struct BuildStats
{
    std::size_t paired { 0 };
    std::size_t unpaired_latitude { 0 };
    std::size_t unpaired_longitude { 0 };
};

// Latitude and longitude, paired BY TIMESTAMP, into `out` (cleared first,
// capacity kept -- the paint pass reuses its scratch).
//
// Not by index. Every position schema in this tree carries latitude and
// longitude on ONE message, and the source stamps one timestamp per message
// which every binding on it shares -- that is the same property that makes two
// fields of a topic line up under the shared cursor. So equal timestamps mean
// "the same fix", and a two-pointer merge over two non-decreasing histories
// pairs them exactly.
//
// The tolerance is a guard, not a feature: the timestamps being compared are
// copies of one number, so exact equality is the intent. A source that ever
// stamped per binding instead of per message would produce an EMPTY track under
// exact equality, and an empty track looks like no data at all rather than like
// a bug -- hence a window narrower than any real sample interval.
inline constexpr double kPairToleranceSeconds = 1e-9;

BuildStats build(const SampleHistory& latitude, const SampleHistory& longitude,
                 const SampleHistory* color, std::vector<Point>& out);

// Drop points that would land within `min_px` of the previously kept one.
//
// A 300 s window at 10 Hz is 3000 points and most of them share a pixel; at a
// continental zoom nearly all of them do. Thinning is in SCREEN space because
// that is where the redundancy is -- a world-space threshold would thin a
// motorway and a car park by the same amount.
//
// The LAST point is always kept when the input is non-empty: it is where the
// vehicle ended up, and dropping it moves the end of the track.
void thin(const std::vector<Point>& in, const map_render::Projection& projection, double min_px,
          std::vector<Point>& out);

// The index of the point nearest `screen`, within `radius_px`, or nothing.
//
// Nearest rather than first-within-radius: a track that doubles back on itself
// -- a lap, a there-and-back -- has two points under the pointer, and picking
// whichever came first in the buffer would seek to the wrong lap.
std::optional<std::size_t> nearest(const std::vector<Point>& points,
                                   const map_render::Projection& projection,
                                   const map_render::ScreenPoint& screen, double radius_px);

// The index of the point at or before `t`, or nothing when `t` is before the
// track starts. Binary search; `points` is non-decreasing in t by construction.
//
// Zero-order hold, never interpolated, for the reason the table's readout gives:
// a position between two fixes is somewhere nothing reported being.
std::optional<std::size_t> at(const std::vector<Point>& points, double t);

}  // namespace scope::track

#endif  // SCOPE_MAP_TRACK_BUILDER_H_
