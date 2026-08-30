// SPDX-License-Identifier: GPL-3.0-or-later
//
// Web Mercator, and the tile arithmetic on top of it.
//
// Pure maths: no Qt, no zenoh, no tiles. That is deliberate, because every
// mistake possible here produces a map that renders perfectly and is in the
// wrong place -- a sign flip puts you in the southern hemisphere, a missing
// 2^zoom puts you off the edge of the world, and neither throws. The only way
// to catch those is a test with hand-computed numbers in it, which is what
// test_projection.cpp is.
//
// Three conventions, held to throughout:
//
//   * WORLD coordinates are normalised: (0,0) is the north-west corner of the
//     whole map and (1,1) the south-east, at every zoom. Scaling by 2^zoom
//     gives tile coordinates and by 2^zoom * tileSize gives pixels. Keeping the
//     unscaled form means the zoom appears exactly once in each conversion.
//
//   * SCREEN coordinates are LOGICAL widget pixels, y down, origin top-left --
//     the units QPainter and every widget coordinate are already in. A
//     projection also carries the device pixel ratio, but does not apply it:
//     the passes that draw through QPainter must stay logical, and the GPU
//     pass is the only one that renders at the screen's real resolution.
//
//   * Latitude is clamped to +/-85.0511287798, where Mercator's tan() runs to
//     infinity. Web Mercator is defined only inside that, and it is what makes
//     the world square.
#ifndef MAP_PROJECTION_H
#define MAP_PROJECTION_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace map_render
{

// The latitude beyond which Web Mercator is undefined. atan(sinh(pi)) in
// degrees -- the value that makes the projected world exactly square.
inline constexpr double kMaxLatitude = 85.0511287798066;

// The deepest tile level this projection is defined for. At z22 a tile is about
// 9 m across and the whole world is 2^22 tiles a side, which is where the
// 64-bit tile hash and the float32 tile-local coordinates both stop being
// comfortable. No archive in this tree goes near it; it is the ceiling used
// before a server has said what its archive actually holds.
inline constexpr std::uint8_t kMaxTileZoom = 22;

// The steepest camera pitch, in degrees off straight-down. The cap is what
// keeps the projection total: at 60 degrees the horizon line sits 0.866
// viewport-heights above the centre while the top edge is only 0.5 above it,
// so every screen pixel still meets the ground plane and worldForScreen()
// never has to answer for a ray that misses the world. (The geometry holds to
// 71.6 degrees at the 1.5x focal length below; 60 leaves headroom.)
inline constexpr double kMaxPitch = 60.0;

struct WorldPoint
{
    // Both in [0, 1] for points on the map. x wraps; y does not, and a y
    // outside [0,1] means a latitude past the Mercator limit.
    double x { 0.0 };
    double y { 0.0 };
};

struct ScreenPoint
{
    double x { 0.0 };
    double y { 0.0 };
};

struct Coordinate
{
    double latitude { 0.0 };
    double longitude { 0.0 };

    // Exact comparison, deliberately. Callers use this to ask "is this the
    // same camera as last frame", where the values are copies of each other
    // rather than the results of two different computations.
    friend bool operator==(const Coordinate&, const Coordinate&) = default;
};

// One tile of the pyramid, in slippy (XYZ) coordinates -- y counting southward
// from the top, as every URL and every request in this system does. The
// TMS-shaped flip lives in mbtiles::Archive and nowhere else.
struct TileId
{
    std::uint8_t z { 0 };
    std::uint32_t x { 0 };
    std::uint32_t y { 0 };

    friend bool operator==(const TileId&, const TileId&) = default;
};

// Hashable, so a tile cache can be a flat map keyed by id.
struct TileIdHash
{
    std::size_t operator()(const TileId& id) const noexcept
    {
        // z is at most 22, x and y at most 2^22, so all three fit one 64-bit
        // word with room to spare and the hash is exact rather than mixed.
        return (static_cast<std::size_t>(id.z) << 44) ^ (static_cast<std::size_t>(id.x) << 22) ^
               static_cast<std::size_t>(id.y);
    }
};

// --------------------------------------------------------------- conversions

double clampLatitude(double degrees);

// Normalise a longitude into [-180, 180). Longitudes wrap; a camera dragged
// east past the date line must not project off the edge of the world.
double wrapLongitude(double degrees);

WorldPoint worldFor(const Coordinate& coordinate);
Coordinate coordinateFor(const WorldPoint& world);

// ------------------------------------------------------------------- camera

struct Camera
{
    Coordinate center;
    // Fractional. 0 is the whole world in one tile, and each step doubles.
    double zoom { 13.0 };
    // Degrees clockwise from north. The map rotates; the vehicle stays upright.
    double bearing { 0.0 };
    // Degrees the view tilts off straight-down, 0 = the flat map. Clamped to
    // [0, kMaxPitch] by the Projection, not here: the camera is a dumb value
    // and the walk memo compares it exactly.
    double pitch { 0.0 };

    // Exact comparison, deliberately: this asks "are these two copies of each
    // other", the same question Coordinate's own operator== answers, not "are
    // they close". The tile-walk memo keys on it.
    friend bool operator==(const Camera&, const Camera&) = default;
};

// Everything needed to turn a coordinate into a pixel for one frame.
//
// Constructed per paint rather than kept, because it is a value: the camera and
// the viewport are what change, and a stale projection is a map drawn one frame
// behind its own camera.
class Projection
{
  public:
    // `widthPx`/`heightPx` are LOGICAL pixels. `devicePixelRatio` says how many
    // real ones the screen puts in each, and is carried rather than applied --
    // see the note on SCREEN coordinates above.
    //
    // The ratio comes BEFORE the tile size on purpose, so that the common call
    // does not have to name a tile size to reach it. Both are defaulted, so
    // Projection(camera, w, h, 512) would quietly mean a ratio of 512; pass
    // the tile size by name or not at all.
    Projection(const Camera& camera, double widthPx, double heightPx,
               double devicePixelRatio = 1.0, int tileSizePx = 512);

    const Camera& camera() const { return mCamera; }
    double tileSize() const { return mTileSizePx; }
    double viewportWidth() const { return mWidthPx; }
    double viewportHeight() const { return mHeightPx; }
    // Always positive; a non-positive ratio from the window system is taken as
    // 1, because every coordinate here is a divisor away from a zero.
    double devicePixelRatio() const { return mDevicePixelRatio; }

    // The rotation this projection applies taking world to screen: cos/sin of
    // the NEGATED bearing (a bearing of 90, "east is up", turns the map
    // anticlockwise on screen -- see the constructor).
    //
    // Prefer tileTransform() to these. They are still here because a test that
    // predicts where a tile-local point lands has to do the arithmetic itself
    // or it is only checking the code against itself.
    double bearingCos() const { return mCos; }
    double bearingSin() const { return mSin; }

    // Whether this projection tilts at all, and the tilt's precomputed terms.
    // sin/cos are of the CLAMPED pitch; focalPixels() is the perspective
    // distance from the eye to the viewport centre, in logical pixels.
    bool pitched() const { return mPitched; }
    double pitchSin() const { return mPitchSin; }
    double pitchCos() const { return mPitchCos; }
    double focalPixels() const { return mFocalPx; }

    // Pixels across the whole world at this zoom. The single number that ties
    // world coordinates to screen ones.
    double worldPixels() const { return mWorldPixels; }

    ScreenPoint screenFor(const WorldPoint& world) const;
    ScreenPoint screenFor(const Coordinate& coordinate) const;
    WorldPoint worldForScreen(const ScreenPoint& screen) const;
    Coordinate coordinateForScreen(const ScreenPoint& screen) const;

    // The integer zoom whose tiles best match this camera, clamped into what
    // the archive actually has. Rounded rather than truncated: at zoom 13.9 the
    // z14 tiles are far closer to 1:1 than the z13 ones, and drawing z13 there
    // is a visibly soft map.
    std::uint8_t tileZoom(std::uint8_t minZoom, std::uint8_t maxZoom) const;

    // Every tile touching the viewport at `z`, with `marginTiles` of extra ring
    // around it.
    //
    // ROW-MAJOR, and stably so: the same set of tiles comes back in the same
    // order however the camera moved to get there. That matters because this
    // order reaches GpuRenderer, which compares the batch list POSITIONALLY to
    // decide whether its vertex buffer is still good -- so an order that
    // shifted as the camera moved re-uploaded every visible tile's geometry
    // for no change at all. Sort a COPY with sortCentreOutward() when the
    // question is what to fetch first.
    //
    // Under pitch the single-zoom grid gives way to a quadtree descent: the
    // near field gets tiles at `z`, the far field shallower ones, because a
    // z14 tile three streets from the horizon is a few pixels tall and two
    // hundred of them is neither drawable nor fetchable. The order is then
    // depth-first with a fixed child order -- a different order, but equally
    // a pure function of the visible set, which is all the positional
    // comparison needs.
    std::vector<TileId> visibleTiles(std::uint8_t z, int marginTiles = 0) const;

    // Reorder `tiles` nearest-first about the viewport centre. For REQUESTS
    // only: with a bounded number of in-flight fetches, this is what decides
    // that the middle of the map fills before the corners.
    void sortCentreOutward(std::vector<TileId>& tiles) const;

    // The two tile sets a paint needs, from one traversal.
    struct VisibleTiles
    {
        // What the paint pass will draw: the tiles actually touching the
        // viewport.
        std::vector<TileId> drawn;
        // `drawn` plus a ring around it -- what to ASK for, so a pan shows map
        // rather than background at the leading edge. A superset, which is why
        // it is worth returning together rather than walking the grid twice.
        std::vector<TileId> withMargin;
        // The walk hit its hard cap and stopped early, so both lists are
        // partial. Only reachable from a camera far shallower than the archive
        // -- 256x the tiles at four levels out -- but when it happens the map
        // is partly drawn and nothing else says why.
        bool truncated { false };
    };

    // `minLeafZoom` matters only under pitch: the far field is drawn from
    // shallower tiles than `z`, and this is the floor under how shallow --
    // the archive's own minimum, so no leaf names a tile that cannot exist.
    VisibleTiles visibleTilesWithMargin(std::uint8_t z, int marginTiles,
                                        std::uint8_t minLeafZoom = 0) const;

    // Where a whole tile lands on the FLAT screen -- rotation applied, tilt
    // not. Under pitch the GPU applies the tilt as one matrix per frame after
    // this flat placement, and TileTransform::map() applies it per point; a
    // tilted origin here would tilt twice. At pitch 0 flat and true screen
    // coincide, which is every frame today.
    ScreenPoint tileOrigin(const TileId& id) const;
    double tileScreenSize(std::uint8_t z) const;

    // The whole tile-local -> screen transform for one tile, solved once.
    //
    // A tile is square in world space and stays square on screen, so this is a
    // similarity -- a scale, a rotation and an offset -- and every point in the
    // tile goes through the same four numbers. Taking it as an object rather
    // than calling a method per point is the difference between hoisting that
    // setup out of a loop and repeating it: the label pass projects a road's
    // whole polyline, not one anchor.
    //
    // Tile-local coordinates are [0,1] across the tile, which is the same
    // camera-free domain the GPU's vertices use and for the same reason.
    struct TileTransform
    {
        // The similarity: the tile's FLAT-screen origin, and the shared
        // rotation. Under pitch a tilt about the viewport centre follows it,
        // so map() is no longer a similarity -- but the flat half still is,
        // and the unpitched frame (every frame today) still takes only it.
        ScreenPoint origin;
        double size { 0.0 };
        double cos { 1.0 };
        double sin { 0.0 };

        // The tilt, shared by every tile of the frame: the viewport centre it
        // pivots about, sin/cos of the clamped pitch, and the focal length.
        // Meaningful only when pitched; the flag keeps the flat path to one
        // predictable branch, because the label pass calls map() per vertex of
        // every road it considers, per frame, on the GUI thread.
        bool pitched { false };
        double centerX { 0.0 };
        double centerY { 0.0 };
        double pitchSin { 0.0 };
        double pitchCos { 1.0 };
        double focal { 1.0 };

        ScreenPoint map(double lx, double ly) const
        {
            const double sx = lx * size;
            const double sy = ly * size;
            const ScreenPoint flat { origin.x + ((sx * cos) - (sy * sin)),
                                     origin.y + ((sx * sin) + (sy * cos)) };
            if (!pitched)
            {
                return flat;
            }
            const double u = flat.x - centerX;
            const double v = flat.y - centerY;
            const double w = std::max(focal - (v * pitchSin), 0.05 * focal);
            return ScreenPoint { centerX + (u * focal / w),
                                 centerY + (v * pitchCos * focal / w) };
        }

        // How much the tilt shrinks things at this tile-local point: 1 on the
        // flat map, less with distance. What a label's glyphs scale by.
        double scaleAt(double lx, double ly) const
        {
            if (!pitched)
            {
                return 1.0;
            }
            const double sx = lx * size;
            const double sy = ly * size;
            const double v = origin.y + ((sx * sin) + (sy * cos)) - centerY;
            const double w = std::max(focal - (v * pitchSin), 0.05 * focal);
            return focal / w;
        }
    };

    TileTransform tileTransform(const TileId& id) const;

  private:
    // The inclusive tile box covering the viewport, before x is wrapped. y is
    // already clamped to the world; x is not, because a camera at the date line
    // legitimately spans both ends of it.
    struct TileBounds
    {
        std::int64_t firstX { 0 };
        std::int64_t lastX { 0 };
        std::int64_t firstY { 0 };
        std::int64_t lastY { 0 };
        std::int64_t sideTiles { 1 };
    };

    TileBounds tileBounds(std::uint8_t z, int marginTiles) const;

    // The flat half of the projection: rotation and scale, no tilt. What the
    // public screenFor()/worldForScreen() wrap the tilt around, and what
    // tileOrigin() exposes directly for the GPU's own tilt matrix.
    ScreenPoint flatScreenFor(const WorldPoint& world) const;

    // The pitched walk. Works in FLAT screen space, where the viewport is a
    // trapezoid and every tile is still an undistorted rotated square -- the
    // one space where both shapes are exact and convex.
    VisibleTiles pitchedVisibleTiles(std::uint8_t z, int marginTiles,
                                     std::uint8_t minLeafZoom) const;

    Camera mCamera;
    double mWidthPx;
    double mHeightPx;
    double mDevicePixelRatio;
    double mTileSizePx;
    double mWorldPixels;
    WorldPoint mCenterWorld;
    // cos/sin of the bearing, computed once. A bearing of zero is the common
    // case and short-circuits.
    double mCos { 1.0 };
    double mSin { 0.0 };
    bool mRotated { false };
    // The tilt: sin/cos of the clamped pitch, and the focal length in logical
    // pixels. 1.5 viewport-heights is MapLibre's cameraToCenterDistance, kept
    // for the same reason it is standard -- the perspective reads as a view of
    // a map, not a view down a corridor.
    double mPitchSin { 0.0 };
    double mPitchCos { 1.0 };
    double mFocalPx { 1.0 };
    bool mPitched { false };
};

// ------------------------------------------------------- standing in for a tile

// How many levels up substituteTiles() will look for a stand-in.
//
// Five is about four thousand times magnified, past which an ancestor is a
// coloured smear rather than a map and is worse than the background it would
// replace.
inline constexpr int kMaxSubstituteLevelsUp = 5;

// What to draw UNDER `wanted` while some of it is still in flight.
//
// Drawing only what has arrived is what makes a map flash its background on
// every zoom, and it reads as a fault rather than as loading. The data to avoid
// that is usually already in hand: a tile cache keyed across zooms still holds
// the shallower tiles you zoomed in from and the deeper ones you zoomed out of,
// and a tile drawn at a zoom other than its own needs no special handling --
// tileOrigin() and tileScreenSize() place it correctly from its own id.
//
// `have(i)` says whether wanted[i] has arrived -- a predicate rather than a
// vector<bool>, because every caller already holds that answer in its own
// per-tile state and was rebuilding a parallel bool vector per frame just to
// feed this. A predicate also removes the length-mismatch failure class the
// vector had. `drawable` answers whether some OTHER tile is cached and has
// geometry worth drawing -- not the same question as "is it cached", because
// an absent tile is cached too, with nothing in it, so that it is not asked
// for again.
//
// Ancestors are preferred over descendants: one covers a tile and its three
// siblings, so it is one draw call for four. Descendants are the zoom-OUT case,
// and they PARTITION their parent, so unlike an ancestor they overlay nothing.
//
// Deduplicated, and capped at `budget` because the renderer draws a bounded
// number of tiles per frame and truncates the tail -- the real tiles must not
// lose their slots to stand-ins. What is dropped when the cap bites is the end
// of the row-major order, i.e. the bottom right of the viewport.
std::vector<TileId> substituteTiles(const std::vector<TileId>& wanted,
                                    const std::function<bool(std::size_t)>& have,
                                    const std::function<bool(const TileId&)>& drawable,
                                    std::size_t budget);

} // namespace map_render

#endif // MAP_PROJECTION_H
