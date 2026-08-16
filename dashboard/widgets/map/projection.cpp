// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/projection.h"

#include <algorithm>
#include <cmath>

namespace map_widget
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

// A hard ceiling on how many tiles one frame may ask for. A viewport is a few
// dozen; anything near this means the zoom or the viewport is nonsense, and
// without a cap the loop below allocates until it cannot.
constexpr std::size_t kMaxVisibleTiles = 4096;

} // namespace

double clampLatitude(double degrees)
{
    return std::clamp(degrees, -kMaxLatitude, kMaxLatitude);
}

double wrapLongitude(double degrees)
{
    // fmod alone leaves the sign of the input, so -190 becomes -190 rather than
    // 170. The extra 360-and-fmod normalises to [0, 360) first.
    double wrapped = std::fmod(degrees + 180.0, 360.0);
    if (wrapped < 0.0)
    {
        wrapped += 360.0;
    }
    return wrapped - 180.0;
}

WorldPoint worldFor(const Coordinate& coordinate)
{
    const double longitude = wrapLongitude(coordinate.longitude);
    const double latitude = clampLatitude(coordinate.latitude);

    const double x = (longitude + 180.0) / 360.0;

    // The Mercator y. Written as log(tan + sec) rather than the equivalent
    // log(tan(pi/4 + lat/2)) because the latter loses precision near the
    // equator, where a dashboard spends none of its time but a test does.
    const double latitudeRad = latitude * kDegToRad;
    const double mercator = std::log(std::tan(latitudeRad) + (1.0 / std::cos(latitudeRad)));
    const double y = 0.5 - (mercator / (2.0 * kPi));

    return WorldPoint { x, y };
}

Coordinate coordinateFor(const WorldPoint& world)
{
    const double longitude = (world.x * 360.0) - 180.0;

    const double mercator = (0.5 - world.y) * 2.0 * kPi;
    const double latitude = std::atan(std::sinh(mercator)) * kRadToDeg;

    return Coordinate { latitude, longitude };
}

Projection::Projection(const Camera& camera, double widthPx, double heightPx, int tileSizePx) :
    mCamera(camera),
    mWidthPx(widthPx),
    mHeightPx(heightPx),
    mTileSizePx(static_cast<double>(tileSizePx)),
    mWorldPixels(std::exp2(camera.zoom) * static_cast<double>(tileSizePx)),
    mCenterWorld(worldFor(camera.center))
{
    const double bearing = std::fmod(camera.bearing, 360.0);
    mRotated = std::abs(bearing) > 1e-9;
    if (mRotated)
    {
        // Negated: a bearing of 90 means "east is up", which rotates the map
        // anticlockwise on screen.
        const double radians = -bearing * kDegToRad;
        mCos = std::cos(radians);
        mSin = std::sin(radians);
    }
}

ScreenPoint Projection::screenFor(const WorldPoint& world) const
{
    // Offset from the camera centre, in pixels, before rotation.
    double dx = (world.x - mCenterWorld.x) * mWorldPixels;
    const double dy = (world.y - mCenterWorld.y) * mWorldPixels;

    // Take the short way round the world. Without this, a camera at 179E and a
    // point at 179W are a whole world apart in x and the point lands off
    // screen instead of just beside the camera.
    if (dx > mWorldPixels / 2.0)
    {
        dx -= mWorldPixels;
    }
    else if (dx < -mWorldPixels / 2.0)
    {
        dx += mWorldPixels;
    }

    if (!mRotated)
    {
        return ScreenPoint { (mWidthPx / 2.0) + dx, (mHeightPx / 2.0) + dy };
    }

    return ScreenPoint { (mWidthPx / 2.0) + (dx * mCos) - (dy * mSin),
                         (mHeightPx / 2.0) + (dx * mSin) + (dy * mCos) };
}

ScreenPoint Projection::screenFor(const Coordinate& coordinate) const
{
    return screenFor(worldFor(coordinate));
}

WorldPoint Projection::worldForScreen(const ScreenPoint& screen) const
{
    double dx = screen.x - (mWidthPx / 2.0);
    double dy = screen.y - (mHeightPx / 2.0);

    if (mRotated)
    {
        // The inverse rotation. mSin is negated rather than recomputed, which
        // is the transpose of a rotation matrix and therefore its inverse.
        const double rx = (dx * mCos) + (dy * mSin);
        const double ry = (-dx * mSin) + (dy * mCos);
        dx = rx;
        dy = ry;
    }

    return WorldPoint { mCenterWorld.x + (dx / mWorldPixels),
                        mCenterWorld.y + (dy / mWorldPixels) };
}

Coordinate Projection::coordinateForScreen(const ScreenPoint& screen) const
{
    return coordinateFor(worldForScreen(screen));
}

std::uint8_t Projection::tileZoom(std::uint8_t minZoom, std::uint8_t maxZoom) const
{
    if (minZoom > maxZoom)
    {
        std::swap(minZoom, maxZoom);
    }

    // A tile is drawn at tileSize pixels when the camera zoom equals the tile
    // zoom. Rounding rather than truncating keeps the map crisp: at zoom 13.9
    // the z14 tiles are near 1:1 and the z13 ones are drawn at double size,
    // which is visibly soft.
    const double wanted = std::round(mCamera.zoom);
    const double clamped =
        std::clamp(wanted, static_cast<double>(minZoom), static_cast<double>(maxZoom));
    return static_cast<std::uint8_t>(clamped);
}

double Projection::tileScreenSize(std::uint8_t z) const
{
    return mWorldPixels / std::exp2(static_cast<double>(z));
}

ScreenPoint Projection::tileOrigin(const TileId& id) const
{
    const double side = std::exp2(static_cast<double>(id.z));
    return screenFor(WorldPoint { static_cast<double>(id.x) / side,
                                  static_cast<double>(id.y) / side });
}

Projection::TileBounds Projection::tileBounds(std::uint8_t z, int marginTiles) const
{
    const double side = std::exp2(static_cast<double>(z));
    const auto sideTiles = static_cast<std::int64_t>(side);

    // The viewport's world-space bounding box. With rotation the axis-aligned
    // box of the four rotated corners is what covers it -- projecting only two
    // corners would leave triangles of the screen untiled whenever the map is
    // turned.
    const ScreenPoint corners[] = { { 0.0, 0.0 },
                                    { mWidthPx, 0.0 },
                                    { mWidthPx, mHeightPx },
                                    { 0.0, mHeightPx } };

    double minX = 1e18;
    double maxX = -1e18;
    double minY = 1e18;
    double maxY = -1e18;
    for (const ScreenPoint& corner : corners)
    {
        const WorldPoint world = worldForScreen(corner);
        minX = std::min(minX, world.x);
        maxX = std::max(maxX, world.x);
        minY = std::min(minY, world.y);
        maxY = std::max(maxY, world.y);
    }

    TileBounds bounds;
    bounds.sideTiles = sideTiles;
    bounds.firstX = static_cast<std::int64_t>(std::floor(minX * side)) - marginTiles;
    bounds.lastX = static_cast<std::int64_t>(std::floor(maxX * side)) + marginTiles;
    bounds.firstY = static_cast<std::int64_t>(std::floor(minY * side)) - marginTiles;
    bounds.lastY = static_cast<std::int64_t>(std::floor(maxY * side)) + marginTiles;

    // y does not wrap: above the north edge and below the south there is
    // nothing, and asking for y = -1 would be a request no archive can answer.
    bounds.firstY = std::max<std::int64_t>(bounds.firstY, 0);
    bounds.lastY = std::min<std::int64_t>(bounds.lastY, sideTiles - 1);

    return bounds;
}

std::vector<TileId> Projection::visibleTiles(std::uint8_t z, int marginTiles) const
{
    return visibleTilesWithMargin(z, marginTiles).withMargin;
}

Projection::VisibleTiles Projection::visibleTilesWithMargin(std::uint8_t z, int marginTiles) const
{
    VisibleTiles out;

    const TileBounds ring = tileBounds(z, marginTiles);
    // The un-margined box, so the ring can be told from what is actually on
    // screen without walking the grid a second time.
    const TileBounds inner = tileBounds(z, 0);

    if (ring.firstY > ring.lastY || ring.lastX < ring.firstX)
    {
        return out;
    }

    for (std::int64_t y = ring.firstY; y <= ring.lastY; ++y)
    {
        for (std::int64_t x = ring.firstX; x <= ring.lastX; ++x)
        {
            // x wraps. A camera near the date line legitimately sees tiles from
            // both ends of the world, and the modulo is what turns -1 into the
            // last column rather than into a request that cannot be served.
            std::int64_t wrapped = x % ring.sideTiles;
            if (wrapped < 0)
            {
                wrapped += ring.sideTiles;
            }

            const TileId id { z, static_cast<std::uint32_t>(wrapped),
                              static_cast<std::uint32_t>(y) };
            out.withMargin.push_back(id);

            // Tested on the UNWRAPPED x, against the un-margined box: after the
            // modulo a ring tile on one side of the date line is
            // indistinguishable from a drawn tile on the other.
            if (x >= inner.firstX && x <= inner.lastX && y >= inner.firstY && y <= inner.lastY)
            {
                out.drawn.push_back(id);
            }

            if (out.withMargin.size() >= kMaxVisibleTiles)
            {
                return out;
            }
        }
    }

    return out;
}

void Projection::sortCentreOutward(std::vector<TileId>& tiles) const
{
    if (tiles.empty())
    {
        return;
    }

    // The camera centre in tile units at whatever zoom these tiles are.
    const double side = std::exp2(static_cast<double>(tiles.front().z));
    const double centreX = mCenterWorld.x * side;
    const double centreY = mCenterWorld.y * side;

    // stable_sort, so two tiles the same distance out keep the row-major order
    // they came in with rather than swapping on a floating-point tie.
    std::stable_sort(tiles.begin(), tiles.end(), [&](const TileId& a, const TileId& b) {
        const double ax = (static_cast<double>(a.x) + 0.5) - centreX;
        const double ay = (static_cast<double>(a.y) + 0.5) - centreY;
        const double bx = (static_cast<double>(b.x) + 0.5) - centreX;
        const double by = (static_cast<double>(b.y) + 0.5) - centreY;
        return ((ax * ax) + (ay * ay)) < ((bx * bx) + (by * by));
    });
}

} // namespace map_widget
