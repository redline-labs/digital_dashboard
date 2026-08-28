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

    VisibleTiles visibleTilesWithMargin(std::uint8_t z, int marginTiles) const;

    // Where a whole tile lands on screen: its north-west corner, and how many
    // pixels across it is. A tile is square in world space and stays square on
    // screen, which is what lets a feature inside it be placed by a single
    // scale and offset rather than a full transform per point.
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
        ScreenPoint origin;
        double size { 0.0 };
        double cos { 1.0 };
        double sin { 0.0 };

        ScreenPoint map(double lx, double ly) const
        {
            const double sx = lx * size;
            const double sy = ly * size;
            return ScreenPoint { origin.x + ((sx * cos) - (sy * sin)),
                                 origin.y + ((sx * sin) + (sy * cos)) };
        }

        // A DIRECTION rather than a position: rotated and scaled, but not
        // offset. What an on-screen bearing along a road is derived from.
        ScreenPoint mapDelta(double dx, double dy) const
        {
            const double sx = dx * size;
            const double sy = dy * size;
            return ScreenPoint { (sx * cos) - (sy * sin), (sx * sin) + (sy * cos) };
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
// `have[i]` says whether wanted[i] has arrived. `drawable` answers whether some
// OTHER tile is cached and has geometry worth drawing -- not the same question
// as "is it cached", because an absent tile is cached too, with nothing in it,
// so that it is not asked for again.
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
                                    const std::vector<bool>& have,
                                    const std::function<bool(const TileId&)>& drawable,
                                    std::size_t budget);

} // namespace map_render

#endif // MAP_PROJECTION_H
