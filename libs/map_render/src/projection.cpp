// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_render/projection.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_set>

namespace map_render
{
namespace
{

constexpr double kPi = std::numbers::pi;
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

Projection::Projection(const Camera& camera, double widthPx, double heightPx,
                       double devicePixelRatio, int tileSizePx) :
    mCamera(camera),
    mWidthPx(widthPx),
    mHeightPx(heightPx),
    mDevicePixelRatio(devicePixelRatio > 0.0 ? devicePixelRatio : 1.0),
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

    // Clamped here rather than in the Camera, which stays a dumb value the
    // walk memo can compare exactly. Below zero makes no sense (the tilt has
    // no meaning upward); above kMaxPitch the top of the screen leaves the
    // ground plane and worldForScreen() stops being total.
    const double pitch = std::clamp(camera.pitch, 0.0, kMaxPitch);
    mPitched = pitch > 1e-9;
    if (mPitched)
    {
        mPitchSin = std::sin(pitch * kDegToRad);
        mPitchCos = std::cos(pitch * kDegToRad);
        // 1.5 viewport-heights: MapLibre's cameraToCenterDistance. The one
        // number that decides how strong the perspective looks.
        mFocalPx = 1.5 * heightPx;
    }
}

ScreenPoint Projection::flatScreenFor(const WorldPoint& world) const
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

ScreenPoint Projection::screenFor(const WorldPoint& world) const
{
    const ScreenPoint flat = flatScreenFor(world);
    if (!mPitched)
    {
        return flat;
    }

    // The tilt, about the viewport centre: rows above the centre recede
    // (divided by a growing w), rows below approach. The w clamp keeps the
    // function total for points far behind the camera; anything it touches
    // lands more than 1.7 viewport-heights below the bottom edge, where every
    // bounds test already rejects it.
    const double u = flat.x - (mWidthPx / 2.0);
    const double v = flat.y - (mHeightPx / 2.0);
    const double w = std::max(mFocalPx - (v * mPitchSin), 0.05 * mFocalPx);
    return ScreenPoint { (mWidthPx / 2.0) + (u * mFocalPx / w),
                         (mHeightPx / 2.0) + (v * mPitchCos * mFocalPx / w) };
}

ScreenPoint Projection::screenFor(const Coordinate& coordinate) const
{
    return screenFor(worldFor(coordinate));
}

WorldPoint Projection::worldForScreen(const ScreenPoint& screen) const
{
    double dx = screen.x - (mWidthPx / 2.0);
    double dy = screen.y - (mHeightPx / 2.0);

    if (mPitched)
    {
        // The tilt's exact inverse, derived by solving the forward form for v.
        // The denominator is zero where the screen row meets the horizon --
        // above the top edge for any pitch within kMaxPitch, which is what
        // keeps this total (see kMaxPitch).
        const double v = dy * mFocalPx / ((mFocalPx * mPitchCos) + (dy * mPitchSin));
        const double w = mFocalPx - (v * mPitchSin);
        dx = dx * w / mFocalPx;
        dy = v;
    }

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
    // Flat on purpose -- see the header: the GPU and TileTransform apply the
    // tilt themselves, downstream of this placement.
    const double side = std::exp2(static_cast<double>(id.z));
    return flatScreenFor(WorldPoint { static_cast<double>(id.x) / side,
                                      static_cast<double>(id.y) / side });
}

Projection::TileTransform Projection::tileTransform(const TileId& id) const
{
    return TileTransform { tileOrigin(id), tileScreenSize(id.z), mCos,     mSin,
                           mPitched,       mWidthPx / 2.0,       mHeightPx / 2.0,
                           mPitchSin,      mPitchCos,            mFocalPx };
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

Projection::VisibleTiles Projection::visibleTilesWithMargin(std::uint8_t z, int marginTiles,
                                                            std::uint8_t minLeafZoom) const
{
    if (mPitched)
    {
        return pitchedVisibleTiles(z, marginTiles, minLeafZoom);
    }

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
                out.truncated = true;
                return out;
            }
        }
    }

    return out;
}


namespace
{

// A convex quad, wound consistently, for the separating-axis test below.
struct Quad
{
    ScreenPoint p[4];
};

// Convex-convex overlap by separating axes: the two quads overlap unless some
// edge normal of either separates their projections. Exact for the shapes the
// pitched walk feeds it -- FLAT-screen tiles are rotated squares and the
// viewport is a trapezoid, both genuinely convex -- which matters because a
// false miss here is a hole in the map.
bool quadsOverlap(const Quad& a, const Quad& b)
{
    const Quad* quads[2] = { &a, &b };
    for (const Quad* quad : quads)
    {
        for (int i = 0; i < 4; ++i)
        {
            const ScreenPoint& from = quad->p[i];
            const ScreenPoint& to = quad->p[(i + 1) % 4];
            const double axisX = to.y - from.y;
            const double axisY = from.x - to.x;

            double minA = 1e300;
            double maxA = -1e300;
            double minB = 1e300;
            double maxB = -1e300;
            for (int k = 0; k < 4; ++k)
            {
                const double projA = (a.p[k].x * axisX) + (a.p[k].y * axisY);
                minA = std::min(minA, projA);
                maxA = std::max(maxA, projA);
                const double projB = (b.p[k].x * axisX) + (b.p[k].y * axisY);
                minB = std::min(minB, projB);
                maxB = std::max(maxB, projB);
            }
            if (maxA < minB || maxB < minA)
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

Projection::VisibleTiles Projection::pitchedVisibleTiles(std::uint8_t z, int marginTiles,
                                                         std::uint8_t minLeafZoom) const
{
    VisibleTiles out;

    const double cx = mWidthPx / 2.0;
    const double cy = mHeightPx / 2.0;

    // The un-tilt, from screen back to the flat plane -- the same inverse
    // worldForScreen applies, reused here on the viewport's own corners.
    const auto flatForScreen = [&](double sx, double sy) {
        const double dy = sy - cy;
        const double v = dy * mFocalPx / ((mFocalPx * mPitchCos) + (dy * mPitchSin));
        const double w = mFocalPx - (v * mPitchSin);
        return ScreenPoint { cx + ((sx - cx) * w / mFocalPx), cy + v };
    };

    // The prefetch margin, inflated in screen pixels. Vertically UP it is
    // clamped to halfway from the top edge to the horizon line: past the
    // horizon there is no ground to prefetch, and even reaching for it makes
    // the flat trapezoid grow without bound.
    const double margin = static_cast<double>(marginTiles) * mTileSizePx;
    const double horizonY = cy - (mFocalPx * mPitchCos / mPitchSin);
    const double top = std::max(-margin, (0.0 + horizonY) / 2.0);

    const auto trapezoidFor = [&](double inflate, double topEdge) {
        return Quad { { flatForScreen(-inflate, topEdge), flatForScreen(mWidthPx + inflate, topEdge),
                        flatForScreen(mWidthPx + inflate, mHeightPx + inflate),
                        flatForScreen(-inflate, mHeightPx + inflate) } };
    };
    const Quad drawnTrapezoid = trapezoidFor(0.0, 0.0);
    const Quad marginTrapezoid =
        marginTiles > 0 ? trapezoidFor(margin, top) : drawnTrapezoid;

    // How far down the flat plane the viewport reaches -- the scale used in
    // the leaf test is capped here, because the part of a tile that pokes
    // below the bottom edge is not drawn and must not keep the tile
    // subdividing.
    double lowestVisibleV = -1e300;
    for (const ScreenPoint& corner : marginTrapezoid.p)
    {
        lowestVisibleV = std::max(lowestVisibleV, corner.y - cy);
    }

    const std::uint8_t targetZ = std::max(z, minLeafZoom);
    // The same threshold the flat walk's round() implies: descend while a tile
    // would be drawn wider than tileSize * sqrt(2), the point where the next
    // level down is the closer match.
    const double splitAt = mTileSizePx * std::numbers::sqrt2;

    // Depth-first, fixed child order. The order is a pure function of the
    // emitted set, which is what the renderer's positional batch comparison
    // needs; see the header.
    struct Node
    {
        TileId id;
    };
    std::vector<Node> stack;
    stack.push_back(Node { TileId { 0, 0, 0 } });

    while (!stack.empty())
    {
        const TileId id = stack.back().id;
        stack.pop_back();

        // The tile's FLAT-screen quad: origin via the (wrapped) flat
        // projection, the other corners by the similarity's own axes so all
        // four agree about which side of the date line they are on.
        const double flatSize = tileScreenSize(id.z);
        const double side = std::exp2(static_cast<double>(id.z));
        const ScreenPoint origin = flatScreenFor(WorldPoint {
            static_cast<double>(id.x) / side, static_cast<double>(id.y) / side });
        const double ax = flatSize * mCos;
        const double ay = flatSize * mSin;
        const Quad quad { { origin,
                            { origin.x + ax, origin.y + ay },
                            { origin.x + ax - ay, origin.y + ay + ax },
                            { origin.x - ay, origin.y + ax } } };

        if (!quadsOverlap(quad, marginTrapezoid))
        {
            continue;
        }

        // The drawn size this tile would have, at the nearest-to-the-eye point
        // of its VISIBLE part: flat v capped at the bottom of the viewport.
        double nearestV = -1e300;
        for (const ScreenPoint& corner : quad.p)
        {
            nearestV = std::max(nearestV, corner.y - cy);
        }
        nearestV = std::min(nearestV, lowestVisibleV);
        const double w = std::max(mFocalPx - (nearestV * mPitchSin), 0.05 * mFocalPx);
        const double drawnSize = flatSize * mFocalPx / w;

        const bool wantsSplit = id.z < targetZ && (drawnSize > splitAt || id.z < minLeafZoom);
        if (wantsSplit)
        {
            // Pushed in reverse so they POP in (0,0), (1,0), (0,1), (1,1)
            // order -- the fixed order the determinism promise names.
            const auto childZ = static_cast<std::uint8_t>(id.z + 1);
            stack.push_back(Node { TileId { childZ, (id.x << 1) + 1, (id.y << 1) + 1 } });
            stack.push_back(Node { TileId { childZ, id.x << 1, (id.y << 1) + 1 } });
            stack.push_back(Node { TileId { childZ, (id.x << 1) + 1, id.y << 1 } });
            stack.push_back(Node { TileId { childZ, id.x << 1, id.y << 1 } });
            continue;
        }

        out.withMargin.push_back(id);
        if (quadsOverlap(quad, drawnTrapezoid))
        {
            out.drawn.push_back(id);
        }
        if (out.withMargin.size() >= kMaxVisibleTiles)
        {
            out.truncated = true;
            return out;
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

    // Distances in WORLD units, per tile, because a pitched walk hands this a
    // MIXED-zoom list -- tile units at tiles.front().z would size every other
    // level wrong. x takes the short way round the date line, as the
    // projection itself does.
    const auto distanceSq = [this](const TileId& id) {
        const double side = std::exp2(static_cast<double>(id.z));
        double dx = ((static_cast<double>(id.x) + 0.5) / side) - mCenterWorld.x;
        const double dy = ((static_cast<double>(id.y) + 0.5) / side) - mCenterWorld.y;
        if (dx > 0.5)
        {
            dx -= 1.0;
        }
        else if (dx < -0.5)
        {
            dx += 1.0;
        }
        return (dx * dx) + (dy * dy);
    };

    // stable_sort, so two tiles the same distance out keep the row-major order
    // they came in with rather than swapping on a floating-point tie.
    std::stable_sort(tiles.begin(), tiles.end(), [&](const TileId& a, const TileId& b) {
        return distanceSq(a) < distanceSq(b);
    });
}

std::vector<TileId> substituteTiles(const std::vector<TileId>& wanted,
                                    const std::function<bool(std::size_t)>& have,
                                    const std::function<bool(const TileId&)>& drawable,
                                    std::size_t budget)
{
    std::vector<TileId> out;
    if (budget == 0 || !have || !drawable)
    {
        return out;
    }

    std::unordered_set<TileId, TileIdHash> seen;

    for (std::size_t i = 0; i < wanted.size() && out.size() < budget; ++i)
    {
        if (have(i))
        {
            // The real thing is here. Nothing to stand in for, and adding one
            // anyway would draw the same ground twice for no reason.
            continue;
        }

        const TileId& want = wanted[i];

        // Up first, nearest ancestor wins: it is the least magnified of the
        // candidates and covers this tile's siblings too, so four missing tiles
        // usually cost one extra draw call between them.
        bool substituted = false;
        for (int up = 1; up <= kMaxSubstituteLevelsUp && up <= int(want.z); ++up)
        {
            const TileId ancestor { static_cast<std::uint8_t>(int(want.z) - up),
                                    want.x >> up, want.y >> up };
            if (!drawable(ancestor))
            {
                continue;
            }
            if (seen.insert(ancestor).second)
            {
                out.push_back(ancestor);
            }
            substituted = true;
            break;
        }
        if (substituted)
        {
            continue;
        }

        // Then one level down. Only one: two levels is sixteen tiles for one,
        // which spends the frame's whole budget on stand-ins, and a zoom
        // gesture moves a level at a time anyway.
        if (want.z >= kMaxTileZoom)
        {
            continue;
        }
        for (std::uint32_t dy = 0; dy < 2 && out.size() < budget; ++dy)
        {
            for (std::uint32_t dx = 0; dx < 2 && out.size() < budget; ++dx)
            {
                const TileId child { static_cast<std::uint8_t>(want.z + 1),
                                     (want.x << 1) + dx, (want.y << 1) + dy };
                if (drawable(child) && seen.insert(child).second)
                {
                    out.push_back(child);
                }
            }
        }
    }

    return out;
}

} // namespace map_render
