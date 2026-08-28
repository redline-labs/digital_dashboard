// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/map_widget.h"

#include "map_render/labels.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "map/highlight_ids.h"
#include "pub_sub/capnp_payload.h"
#include "pub_sub/raw_subscriber.h"
#include "qt_helpers/widget_colors.h"
#include "road_graph/format.h"

#include <QFont>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QResizeEvent>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <numbers>
#include <unordered_set>

#include <algorithm>
#include <cmath>

namespace
{

// An extra ring of tiles around the viewport, fetched but not visible. It is
// what makes a pan show map rather than background at the leading edge.
constexpr int kPrefetchRingTiles = 1;

// How many zoom levels BELOW the drawn one to fetch as a coarse overview.
//
// The ring above covers a pan of a tile or so. It cannot cover a zoom, and it
// cannot cover a pan into ground the drive has never seen at any zoom -- for
// that, substituteTiles() needs an ancestor in the cache, and the only reason
// one is ever there today is that the camera happened to sit at a shallower
// zoom earlier. Fetching the ancestors outright is what makes a stand-in
// available for ground that was never visited.
//
// Four levels, which is what MapLibre's prefetchZoomDelta defaults to, and the
// arithmetic is why: one overview tile spans 16x16 of the drawn ones, so a
// viewport of forty tiles is covered by one or two. That is a rounding error on
// the request budget in exchange for never blanking.
//
// It must stay within substituteTiles()'s reach -- kMaxSubstituteLevelsUp is 5
// -- or the tiles would be fetched, cached, and never looked at.
constexpr std::uint8_t kOverviewZoomDelta = 4;
static_assert(kOverviewZoomDelta <= map_render::kMaxSubstituteLevelsUp,
              "an overview deeper than substituteTiles() will look is fetched and never drawn");

// How much of a zoom level one detent of a wheel is worth. A whole level per
// notch overshoots badly on the way in -- street to block in one click -- and a
// quarter takes four clicks to do anything.
constexpr double kZoomPerWheelNotch = 0.5;

// A wheel reports in eighths of a degree and a detent is 15 degrees, so 120 is
// one notch. Qt documents this and every mouse in the world follows it.
constexpr double kWheelUnitsPerNotch = 120.0;

// A trackpad sends pixels instead, continuously and in much smaller amounts.
// This is a FEEL constant with no authority behind it: it is how far two
// fingers travel for one notch's worth of zoom. At 120 px -- and half a level
// per notch -- a zoom level costs a deliberate 240 px of swipe, which is about
// as far as two fingers comfortably travel in one go.
constexpr double kTrackpadPixelsPerNotch = 120.0;

// How long the camera eases take. Short for the wheel -- it repeats, and each
// notch retargets the ease in flight -- and a touch longer for the recentre
// fly-back, which is a single deliberate gesture.
constexpr std::chrono::milliseconds kZoomEaseMs { 140 };
constexpr std::chrono::milliseconds kRecentreEaseMs { 300 };

// Smoothstep: eased at both ends, so a camera move neither jerks off the mark
// nor lands with a visible stop.
double easeSmooth(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// 0..1 progress of an ease begun at `start`.
double easeProgress(std::chrono::steady_clock::time_point start,
                    std::chrono::steady_clock::time_point now, std::chrono::milliseconds length)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
    return length.count() <= 0 ? 1.0 : double(elapsed.count()) / double(length.count());
}

} // namespace

MapWidget::MapWidget(const config_t& config, QWidget* parent) :
    QWidget(parent), mConfig(config)
{
    setAutoFillBackground(false);

    // A failure here is not fatal and must not throw: there is no CPU fallback,
    // but a widget that reports "no GPU" in its own frame is far easier to
    // diagnose than one that refuses to construct and takes the layout with it.
    mGpu = map_render::GpuRenderer::create();
    if (!mGpu)
    {
        SPDLOG_ERROR("[map] no QRhi backend; the map will draw labels and marker only");
    }
    else
    {
        SPDLOG_INFO("[map] GPU backend: {}", mGpu->backendName().toStdString());
    }

    // This callback fires ON A ZENOH THREAD. The only safe thing it may do is
    // post to the GUI thread; touching the widget here would race the paint.
    //
    // The exchange is the coalescing: the first tile of a burst posts one
    // invoke, the rest see the flag set and post nothing, and the single
    // handler drains however many arrived. See mDrainPending.
    // The base tileset, then each overlay. One TileSource each: they are
    // separate archives on separate update schedules, and each learns its own
    // zoom range from its own server replies.
    //
    // ONE drain callback shared between them, and the coalescing flag is shared
    // too. A burst arriving from two sources at once must still post one
    // invoke, not two -- the handler drains every source anyway.
    const auto onArrival = [this]() {
        if (mDrainPending.exchange(true))
        {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this]() {
                mDrainPending.store(false);
                // Both, and drain() FIRST so it always runs: a newly learned
                // zoom range changes which level the next paint asks for, and
                // when the camera is parked past the archive that is the only
                // thing that will have changed.
                bool changed = false;
                for (const auto& source : mSources)
                {
                    const auto drained = source->drain();
                    // Failures repaint too, not just arrivals: the paint is
                    // what promotes the caption to "No reply from map_server",
                    // and later, when the backoff timer fires, what asks
                    // again. Skipping it left a map with no position stream
                    // stuck on "Waiting for tiles" forever.
                    changed = changed || drained.arrived > 0 || drained.failed > 0 ||
                              source->takeArchiveRangeLearned();
                }
                if (changed)
                {
                    update();
                }
            },
            Qt::QueuedConnection);
    };

    mSources.push_back(std::make_unique<map_widget::TileSource>(
        mConfig.tileset, mConfig.tile_zenoh_key, mConfig.request_timeout_ms, mConfig.style,
        onArrival));
    for (const std::string& overlay : mConfig.overlay_tilesets)
    {
        if (overlay.empty() || overlay == mConfig.tileset)
        {
            // Naming the base again would double every request and draw every
            // feature twice, which is invisible except as a halved frame rate.
            continue;
        }
        mSources.push_back(std::make_unique<map_widget::TileSource>(
            overlay, mConfig.tile_zenoh_key, mConfig.request_timeout_ms, mConfig.style,
            onArrival));
    }

    if (!mConfig.position_zenoh_key.empty())
    {
        if (!mConfig.latitude_expression.empty())
        {
            mLatitudeSubscription = dashboard::makeExpressionSubscription<double>(
                mConfig.position_schema_type, mConfig.latitude_expression,
                mConfig.position_zenoh_key, this, &MapWidget::setLatitude, "map latitude");
        }
        if (!mConfig.longitude_expression.empty())
        {
            mLongitudeSubscription = dashboard::makeExpressionSubscription<double>(
                mConfig.position_schema_type, mConfig.longitude_expression,
                mConfig.position_zenoh_key, this, &MapWidget::setLongitude, "map longitude");
        }
        if (!mConfig.heading_expression.empty())
        {
            mHeadingSubscription = dashboard::makeExpressionSubscription<double>(
                mConfig.position_schema_type, mConfig.heading_expression,
                mConfig.position_zenoh_key, this, &MapWidget::setHeading, "map heading");
        }

        if (mConfig.rotate_with_heading && mConfig.heading_expression.empty())
        {
            SPDLOG_WARN("[map] rotate_with_heading is set but heading_expression is empty; the "
                        "map will keep the configured bearing");
        }
    }

    if (mConfig.interactive)
    {
        // An open hand is the whole affordance: there is nothing else on a map
        // that says it can be dragged, and a cursor that changes is cheaper
        // than any chrome that would.
        setCursor(Qt::OpenHandCursor);

        mRecentre = new map_widget::RecentreButton(qt_helpers::toQColor(mConfig.style.label_text),
                                                   qt_helpers::toQColor(mConfig.style.label_halo), this);
        mRecentre->hide();
        connect(mRecentre, &QAbstractButton::clicked, this, &MapWidget::recentreCamera);
    }

    if (!mConfig.highlight_zenoh_key.empty())
    {
        // RawSubscriber rather than the expression binding: the payload is a
        // struct and the answer is a LIST of way ids, and
        // ZenohExpressionSubscriber yields a double.
        mHighlightSubscription = std::make_unique<pub_sub::RawSubscriber>(
            mConfig.highlight_zenoh_key,
            [this](const std::vector<std::uint8_t>& bytes, std::string_view schema) {
                // ON A ZENOH RX THREAD. Decode into the mailbox and post;
                // touching Qt here would race the paint.
                if (schema != "MapHorizon")
                {
                    // Decoding against the wrong schema is SILENT -- capnp
                    // reads the same bytes at different offsets and hands back
                    // a plausible wrong answer -- so the publisher's own stamp
                    // is checked rather than trusted.
                    return;
                }

                const pub_sub::WordAlignedPayload payload(
                    reinterpret_cast<const kj::byte*>(bytes.data()), bytes.size());
                if (payload.empty())
                {
                    return;
                }

                std::vector<std::uint64_t> ids;
                try
                {
                    ::capnp::FlatArrayMessageReader reader(payload.words());
                    ids = map_widget::highlightWayIds(reader.getRoot<::MapHorizon>());
                }
                catch (const kj::Exception&)
                {
                    // A malformed message. Dropped: one bad sample must not
                    // take the widget down, and the next is 100 ms away.
                    return;
                }

                {
                    const std::lock_guard<std::mutex> lock(mHighlightMutex);
                    mHighlightMailbox = std::move(ids);
                    mHighlightMailboxFresh = true;
                }
                if (mHighlightPending.exchange(true))
                {
                    return;
                }
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        mHighlightPending.store(false);
                        std::vector<std::uint64_t> drained;
                        bool fresh = false;
                        {
                            const std::lock_guard<std::mutex> lock(mHighlightMutex);
                            fresh = mHighlightMailboxFresh;
                            mHighlightMailboxFresh = false;
                            drained.swap(mHighlightMailbox);
                        }
                        if (fresh)
                        {
                            setHighlightWayIds(std::move(drained));
                        }
                    },
                    Qt::QueuedConnection);
            });
    }

    // The retry wake-up. Single-shot and re-armed only while something is
    // backing off, so a healthy idle map keeps costing nothing. The paint the
    // timeout triggers is what re-issues the request -- see armRetryTimer().
    mRetryTimer.setSingleShot(true);
    connect(&mRetryTimer, &QTimer::timeout, this, qOverload<>(&MapWidget::update));

    // The animation ticker: repaints while a tile is fading in. Single-shot
    // and re-armed only from the end of a paint that drew a fading tile, so
    // it dies by itself the frame after the last fade settles.
    mAnimationTimer.setSingleShot(true);
    mAnimationTimer.setInterval(16);
    connect(&mAnimationTimer, &QTimer::timeout, this, qOverload<>(&MapWidget::update));

    // No refreshTiles() here on purpose. A widget is constructed at Qt's
    // default 640x480 and sized by its layout afterwards, so fetching now would
    // request a dozen tiles for a viewport this widget never has. The paint
    // pass asks for what it is about to draw.
}

MapWidget::~MapWidget() = default;

map_render::Camera MapWidget::camera() const
{
    map_render::Camera out;

    // Three sources, in this order and not another: where the user dragged to,
    // then the vehicle if follow is on, then the configured centre -- which is
    // also what the editor previews. A drag beats Follow Vehicle rather than
    // fighting it, because the alternative is a map that snaps back on the
    // next position fix and cannot be looked away from at all.
    if (mInteractionCentre.has_value())
    {
        out.center = *mInteractionCentre;
    }
    else if (mConfig.follow_vehicle && hasPosition())
    {
        out.center = map_render::Coordinate { *mLatitude, *mLongitude };
    }
    else
    {
        out.center =
            map_render::Coordinate { mConfig.center_latitude, mConfig.center_longitude };
    }

    out.zoom = mInteractionZoom.value_or(mConfig.zoom);
    out.bearing = (mConfig.rotate_with_heading && mHeading.has_value()) ? *mHeading
                                                                       : mConfig.bearing;
    return out;
}

map_render::Projection MapWidget::projectionFor(const QPainter& painter) const
{
    // LOGICAL size plus the ratio, rather than one or the other. Everything
    // this widget draws with QPainter -- labels, the marker, the trail -- is in
    // logical pixels and Qt scales the backing store for them, so a projection
    // in device pixels would put all three at twice their coordinates. The GPU
    // pass is the one that reads the ratio, and it is the one that was
    // rendering at half resolution without it.
    //
    // Read off the PAINT DEVICE rather than the widget, because that is where
    // layOutText() reads it from. The two passes have to agree on the ratio or
    // this bug comes straight back in the other direction -- text rendered for
    // a ratio the geometry underneath was not drawn at -- and the only way to
    // guarantee that is to ask the same object.
    const double ratio =
        painter.device() != nullptr ? painter.device()->devicePixelRatioF() : 1.0;
    return map_render::Projection(camera(), width(), height(), ratio);
}

void MapWidget::refreshTiles(const map_render::Projection& projection)
{
    // CLEARED, not left alone. A widget is constructed at Qt's default size and
    // may then be resized to nothing by a layout that has not run yet; keeping
    // the tiles worked out for the default size would have it claim to need
    // tiles it will never draw, and status() would report a healthy map.
    if (width() <= 0 || height() <= 0 || mSources.empty())
    {
        mVisible.clear();
        // With it goes the truncation flag: it describes the walk that
        // produced mVisible, and status() reporting "knowingly partial" about
        // a viewport that no longer exists would send somebody chasing a cap
        // that is not being hit. The walk memo goes too -- its lists describe
        // the same dead viewport.
        mTileWalkTruncated = false;
        mWalkCamera.reset();
        return;
    }

    // The memo: same camera, same viewport, same archive ranges -- the same
    // walk, so its lists are reused and only request() runs. request() must
    // STILL run every paint: it is what re-asks deferred tiles and expired
    // backoffs, and it is already the cheap early-out when nothing is due.
    const bool sameInputs = [&]() {
        if (!mWalkCamera.has_value() || !(*mWalkCamera == projection.camera()) ||
            mWalkWidth != width() || mWalkHeight != height() ||
            mWalkRanges.size() != mSources.size())
        {
            return false;
        }
        for (std::size_t s = 0; s < mSources.size(); ++s)
        {
            if (mWalkRanges[s] != mSources[s]->archiveZoomRange())
            {
                return false;
            }
        }
        return true;
    }();
    if (sameInputs)
    {
        for (std::size_t s = 0; s < mSources.size(); ++s)
        {
            mSources[s]->request(mRequestLists[s]);
        }
        return;
    }

    mVisible.assign(mSources.size(), {});
    mTileWalkTruncated = false;
    mWalkCamera = projection.camera();
    mWalkWidth = width();
    mWalkHeight = height();
    mWalkRanges.assign(mSources.size(), std::nullopt);
    mRequestLists.resize(mSources.size());
    for (std::size_t s = 0; s < mSources.size(); ++s)
    {
        // The ARCHIVE's range, reported by the server, NOT the configured one --
        // which is the camera's business and may legitimately reach past what any
        // archive holds. Until the first reply lands there is nothing to clamp to,
        // so the whole span is allowed and at most one batch comes back
        // outOfRange; from then on the range is known and it cannot happen again.
        const auto archive = mSources[s]->archiveZoomRange();
        mWalkRanges[s] = archive;
        const std::uint8_t z =
            archive.has_value()
                ? projection.tileZoom(archive->min, archive->max)
                : projection.tileZoom(0, static_cast<std::uint8_t>(map_render::kMaxTileZoom));

        // Both sets from one walk of the grid. The prefetch ring is requested but
        // never drawn, which is what keeps mVisible honest about what the paint
        // pass will look at -- and what status() reports.
        auto tiles = projection.visibleTilesWithMargin(z, kPrefetchRingTiles);
        mVisible[s] = std::move(tiles.drawn);
        mTileWalkTruncated = mTileWalkTruncated || tiles.truncated;

        // Sorted centre-outward HERE and not in mVisible: the request order decides
        // which tiles win the in-flight slots, and the draw order must stay stable
        // or the renderer re-uploads every tile whenever the camera reshuffles it.
        projection.sortCentreOutward(tiles.withMargin);

        // The coarse overview goes on the END of the request list, so it can only
        // ever spend request slots the viewport did not want. A stand-in that
        // arrives at the cost of the real tile it stands in for is not a saving.
        const std::uint8_t archiveMin = archive.has_value() ? archive->min : 0;
        if (z > archiveMin)
        {
            const auto overviewZ =
                static_cast<std::uint8_t>(std::max(int(z) - int(kOverviewZoomDelta), int(archiveMin)));
            for (const map_render::TileId& id : projection.visibleTiles(overviewZ, 0))
            {
                tiles.withMargin.push_back(id);
            }
        }

        mRequestLists[s] = std::move(tiles.withMargin);
        mSources[s]->request(mRequestLists[s]);
    }
}

void MapWidget::resizeEvent(QResizeEvent* event)
{
    // No refreshTiles() here. A resize is always followed by a paint, and the
    // paint pass recomputes at the size actually being drawn -- doing it twice
    // only walked the tile grid twice for the same answer.
    QWidget::resizeEvent(event);
    layOutRecentreButton();
}

// ------------------------------------------------------------------- the mouse

map_render::Projection MapWidget::interactionProjection() const
{
    return map_render::Projection(camera(), width(), height(), devicePixelRatioF());
}

void MapWidget::setInteractionCentreQuiet(const map_render::Coordinate& where)
{
    // Clamped and wrapped HERE rather than trusted. A drag past the top of the
    // world produces a latitude Web Mercator has no answer for, and one across
    // the date line produces a longitude outside [-180, 180) that would project
    // a whole world away. Both functions are the projection's own, so the map
    // stops at the poles and runs continuously round the equator.
    mInteractionCentre = map_render::Coordinate { map_render::clampLatitude(where.latitude),
                                                  map_render::wrapLongitude(where.longitude) };
}

void MapWidget::setInteractionCentre(const map_render::Coordinate& where)
{
    setInteractionCentreQuiet(where);
    layOutRecentreButton();
    update();
}

void MapWidget::moveCameraSoThatQuiet(const map_render::WorldPoint& world, const QPointF& screen)
{
    const map_render::Projection projection = interactionProjection();

    // What is under the pointer NOW, at the camera as it currently stands. The
    // difference between that and where the caller wants it is exactly how far
    // the centre has to move -- in world units, so it is right at every zoom,
    // and through worldForScreen(), so it is right under rotation too. A
    // rotated map dragged with a screen-space delta moves off at an angle to
    // the pointer, and that inverse is already written and tested.
    const map_render::WorldPoint under =
        projection.worldForScreen(map_render::ScreenPoint { screen.x(), screen.y() });
    const map_render::WorldPoint centre = map_render::worldFor(projection.camera().center);

    setInteractionCentreQuiet(map_render::coordinateFor(map_render::WorldPoint {
        centre.x + (world.x - under.x), centre.y + (world.y - under.y) }));
}

void MapWidget::moveCameraSoThat(const map_render::WorldPoint& world, const QPointF& screen)
{
    moveCameraSoThatQuiet(world, screen);
    layOutRecentreButton();
    update();
}

void MapWidget::zoomBy(double levels, const QPointF& at, std::chrono::milliseconds ease)
{
    const map_render::Projection before = interactionProjection();

    // The CAMERA's range, which is the layout's business and has nothing to do
    // with how deep the archive goes. Asking to be closer than the archive can
    // answer is a perfectly reasonable thing to want -- refreshTiles() draws
    // the deepest level there is, magnified, and magnified vector tiles stay
    // sharp.
    const double wanted = std::clamp(before.camera().zoom + levels,
                                     static_cast<double>(mConfig.min_zoom),
                                     static_cast<double>(mConfig.max_zoom));
    if (wanted == before.camera().zoom)
    {
        return;
    }

    // Following the vehicle: zoom about the CENTRE. The vehicle does not move
    // on screen, so there is nothing to suspend and the map keeps tracking --
    // see mInteractionCentre. Anchoring on the pointer here would drag the
    // camera off the vehicle as a side effect of wanting a closer look.
    //
    // Deliberately NOT conditioned on hasPosition(): Follow Vehicle is a
    // declared intent, not a state that waits for a fix. Zooming in while the
    // GPS is still coming up must not quietly cancel it, or the map sits on the
    // configured centre forever and the first position to arrive changes
    // nothing.
    // Eased, not assigned: the zoom glides there over kZoomEaseMs, advanced by
    // tickAnimations() at the top of each paint. A second notch mid-flight
    // retargets from the CURRENT eased value -- `wanted` above was already
    // computed from it -- so repeated scrolling accelerates smoothly instead
    // of queueing jumps.
    if (!mInteractionCentre.has_value() && mConfig.follow_vehicle)
    {
        mZoomEase = ZoomEase { before.camera().zoom, wanted, {}, at, false,
                               std::chrono::steady_clock::now(), ease };
        update();
        return;
    }

    const map_render::WorldPoint anchor =
        before.worldForScreen(map_render::ScreenPoint { at.x(), at.y() });
    mZoomEase = ZoomEase { before.camera().zoom, wanted, anchor, at, true,
                           std::chrono::steady_clock::now(), ease };
    update();
}

void MapWidget::recentreCamera()
{
    if (!mInteractionCentre.has_value())
    {
        layOutRecentreButton();
        update();
        return;
    }
    // Fly back rather than snap: the centre glides toward the live target
    // (see RecentreEase) and mInteractionCentre is reset only on landing.
    // The button hides at once -- see layOutRecentreButton().
    mRecentreEase = RecentreEase { *mInteractionCentre, std::chrono::steady_clock::now() };
    layOutRecentreButton();
    update();
}

void MapWidget::layOutRecentreButton()
{
    if (mRecentre == nullptr)
    {
        return;
    }

    // Bottom right, which is where a map's controls live and, more to the
    // point, the one corner nothing else uses: the diagnostic line is centred
    // and the vehicle marker follows the vehicle.
    const int size = map_widget::RecentreButton::kSize;
    const int margin = map_widget::RecentreButton::kMargin;
    mRecentre->setGeometry(width() - size - margin, height() - size - margin, size, size);

    // Shown only when it has something to undo -- a button that is always
    // there is a button that is usually a lie -- and hidden the moment the
    // fly-back starts, even though mInteractionCentre technically stays set
    // until it lands: mid-flight there is nothing left to press for.
    mRecentre->setVisible(mInteractionCentre.has_value() && !mRecentreEase.has_value() &&
                          width() > (size + (2 * margin)) &&
                          height() > (size + (2 * margin)));
}

void MapWidget::mousePressEvent(QMouseEvent* event)
{
    if (!mConfig.interactive || event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    // The grab wins: a hand on the map stops whatever the camera was doing
    // by itself. The zoom stays wherever the ease had gotten it; a cancelled
    // fly-back leaves the centre where it is, still suspended.
    mZoomEase.reset();
    if (mRecentreEase.has_value())
    {
        mRecentreEase.reset();
        layOutRecentreButton();
    }

    mDragAnchor = interactionProjection().worldForScreen(
        map_render::ScreenPoint { event->position().x(), event->position().y() });
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void MapWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!mDragAnchor.has_value())
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    moveCameraSoThat(*mDragAnchor, event->position());
    event->accept();
}

void MapWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (!mDragAnchor.has_value())
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    mDragAnchor.reset();
    setCursor(Qt::OpenHandCursor);
    event->accept();
}

void MapWidget::wheelEvent(QWheelEvent* event)
{
    if (!mConfig.interactive)
    {
        QWidget::wheelEvent(event);
        return;
    }

    // pixelDelta first, because a device that reports both is a trackpad and
    // its pixels are the finer signal. angleDelta is what a wheel with detents
    // sends and is all it sends.
    const QPoint pixels = event->pixelDelta();
    const bool trackpad = !pixels.isNull();
    const double notches = trackpad
                               ? (static_cast<double>(pixels.y()) / kTrackpadPixelsPerNotch)
                               : (static_cast<double>(event->angleDelta().y()) /
                                  kWheelUnitsPerNotch);
    if (notches == 0.0)
    {
        QWidget::wheelEvent(event);
        return;
    }

    // The ease is for DETENTS. A wheel notch is a discrete jump that would
    // snap without it, and they arrive far enough apart that each one gets to
    // finish. A trackpad is the opposite: it sends a continuous stream every
    // few milliseconds, and each event restarts the ease from the value the
    // last one had reached. easeSmooth() is flat at t=0 -- deliberately, that
    // is what makes a notch start gently -- so an ease that is restarted at
    // 8 ms into its 140 ms never gets past the first 1% of its travel, and the
    // gesture delivers a hundredth of the zoom the fingers asked for. The
    // stream is already smooth; ease it over nothing and let each delta land.
    zoomBy(notches * kZoomPerWheelNotch, event->position(),
           trackpad ? std::chrono::milliseconds { 0 } : kZoomEaseMs);
    event->accept();
}

void MapWidget::setLatitude(double degrees)
{
    mLatitude = degrees;
    onPositionChanged();
}

void MapWidget::setLongitude(double degrees)
{
    mLongitude = degrees;
    onPositionChanged();
}

void MapWidget::setHeading(double degrees)
{
    mHeading = degrees;
    if (mConfig.rotate_with_heading)
    {
        // update() only: the paint pass refreshes the tile set itself, at the
        // camera it is actually about to draw.
        update();
    }
}

void MapWidget::onPositionChanged()
{
    if (!hasPosition())
    {
        return;
    }

    const map_render::Coordinate here { *mLatitude, *mLongitude };

    if (mConfig.show_track && mConfig.track_points > 0)
    {
        mTrack.push_back(map_render::worldFor(here));
        while (mTrack.size() > mConfig.track_points)
        {
            mTrack.pop_front();
        }
    }

    update();
}

bool MapWidget::tickAnimations(std::chrono::steady_clock::time_point now)
{
    bool animating = false;

    if (mZoomEase.has_value())
    {
        const double t = easeProgress(mZoomEase->start, now, mZoomEase->length);
        mInteractionZoom =
            mZoomEase->from + ((mZoomEase->to - mZoomEase->from) * easeSmooth(t));
        if (mZoomEase->anchored)
        {
            // Re-solved at EVERY eased step: zoom-about-the-pointer is a
            // property of the whole gesture, not of its endpoints. The world
            // point grabbed at the first notch stays pinned under the pointer
            // while the scale glides.
            moveCameraSoThatQuiet(mZoomEase->anchorWorld, mZoomEase->anchorScreen);
        }
        if (t >= 1.0)
        {
            mInteractionZoom = mZoomEase->to;
            mZoomEase.reset();
        }
        else
        {
            animating = true;
        }
    }

    if (mRecentreEase.has_value())
    {
        // The LIVE target, re-read each tick: a moving vehicle is flown TO,
        // not to where it was when the button was pressed.
        const map_render::Coordinate target =
            (mConfig.follow_vehicle && hasPosition())
                ? map_render::Coordinate { *mLatitude, *mLongitude }
                : map_render::Coordinate { mConfig.center_latitude, mConfig.center_longitude };

        const double t = easeProgress(mRecentreEase->start, now, kRecentreEaseMs);
        if (t >= 1.0)
        {
            // Landed: normal follow resumes, exactly as the instant recentre
            // used to leave things.
            mInteractionCentre.reset();
            mRecentreEase.reset();
        }
        else
        {
            // Interpolated in world space, where a straight line is straight
            // on the map -- lerping degrees bends near the poles and across
            // the date line.
            const map_render::WorldPoint a = map_render::worldFor(mRecentreEase->from);
            const map_render::WorldPoint b = map_render::worldFor(target);
            const double k = easeSmooth(t);
            setInteractionCentreQuiet(map_render::coordinateFor(map_render::WorldPoint {
                a.x + ((b.x - a.x) * k), a.y + ((b.y - a.y) * k) }));
            animating = true;
        }
    }

    return animating;
}

float MapWidget::tileFadeAlpha(const map_render::TileId& id,
                               std::chrono::steady_clock::time_point now)
{
    if (mConfig.tile_fade_ms == 0)
    {
        return 1.0F;
    }
    const auto [at, inserted] = mFirstDrawn.try_emplace(id, now);
    if (inserted)
    {
        return 0.0F;
    }
    const double elapsed =
        double(std::chrono::duration_cast<std::chrono::milliseconds>(now - at->second).count());
    const double t = std::clamp(elapsed / double(mConfig.tile_fade_ms), 0.0, 1.0);
    // Smoothstep: eases both ends, so a fade neither snaps on nor lands with
    // a visible step at full opacity.
    return float(t * t * (3.0 - (2.0 * t)));
}

void MapWidget::assembleBatches()
{
    const auto now = std::chrono::steady_clock::now();
    mLastTilesFading = 0;
    mBatches.clear();
    mLabelTiles.clear();
    mReady.resize(mSources.size());
    mAlphas.resize(mSources.size());
    mStandIns.resize(mSources.size());
    mStandInTiles.resize(mSources.size());

    // The stand-in budget is SHARED across sources, and counted against what
    // every source together already wants to draw. Working it out per source
    // would let two of them each claim the whole frame's headroom and overrun
    // kMaxTilesPerFrame, which silently drops whatever came last.
    std::size_t visibleTotal = 0;
    for (const auto& ids : mVisible)
    {
        visibleTotal += ids.size();
    }
    std::size_t budget = map_render::GpuRenderer::kMaxTilesPerFrame > visibleTotal
                             ? map_render::GpuRenderer::kMaxTilesPerFrame - visibleTotal
                             : 0;

    for (std::size_t s = 0; s < mSources.size(); ++s)
    {
        mSources[s]->ready(mVisible[s], mReady[s]);
        mAlphas[s].assign(mVisible[s].size(), 1.0F);

        // What the cache can put under the gaps while the real tiles are in
        // flight. See substituteTiles(): without it a zoom blanks the map to
        // its background for as long as the round trip takes, which reads as a
        // fault.
        //
        // A tile still FADING keeps its stand-in too -- that is the trap in
        // the crossfade. Treat it as arrived and the ancestor underneath is
        // dropped the frame the fade starts, so the new tile blends with the
        // BACKGROUND instead of with the picture it is replacing; the map
        // flashes dark precisely where it was supposed to ease over. This is
        // also what carries the half-zoom boundary: the old level is one step
        // away, well inside the substitution's reach.
        mHave.assign(mVisible[s].size(), false);
        for (std::size_t i = 0; i < mVisible[s].size(); ++i)
        {
            if (!mReady[s][i])
            {
                continue;
            }
            mAlphas[s][i] = tileFadeAlpha(mVisible[s][i], now);
            mHave[i] = mAlphas[s][i] >= 1.0F;
            if (!mHave[i])
            {
                ++mLastTilesFading;
            }
        }
        map_widget::TileSource& source = *mSources[s];
        mStandIns[s] = map_render::substituteTiles(
            mVisible[s], mHave, [&source](const map_render::TileId& id) {
                return source.drawable(id);
            },
            budget);
        budget -= std::min(budget, mStandIns[s].size());
        source.ready(mStandIns[s], mStandInTiles[s]);
    }

    // Stand-ins FIRST, so they are drawn UNDER within each layer pass and a
    // real tile that has arrived covers its own ground. They overdraw where an
    // ancestor spans a tile that did arrive, which is harmless: an ancestor is
    // the same geography more simply drawn, so the two coincide.
    //
    // Every source's stand-ins before any source's real tiles, and not
    // per-source, because the renderer draws LAYER-major across the whole batch
    // list -- so within one layer the order here is the only thing deciding
    // what covers what.
    for (std::size_t s = 0; s < mSources.size(); ++s)
    {
        for (std::size_t i = 0; i < mStandIns[s].size(); ++i)
        {
            if (mStandInTiles[s][i])
            {
                // A stand-in fades on its own clock, keyed by its own id -- an
                // ancestor that was on screen moments ago at another zoom is
                // already in mFirstDrawn and draws solid at once.
                mBatches.push_back(map_render::GpuBatch { mStandIns[s][i],
                                                         mStandInTiles[s][i].geometry,
                                                         tileFadeAlpha(mStandIns[s][i], now) });
            }
        }
    }
    mLastTilesStandIn = static_cast<int>(mBatches.size());

    for (std::size_t s = 0; s < mSources.size(); ++s)
    {
        for (std::size_t i = 0; i < mVisible[s].size(); ++i)
        {
            if (!mReady[s][i])
            {
                continue;
            }
            mBatches.push_back(map_render::GpuBatch { mVisible[s][i], mReady[s][i].geometry,
                                                     mAlphas[s][i] });
            mLabelTiles.push_back(map_render::LabelTile { mVisible[s][i], mReady[s][i].labels });
        }
    }
    mLastTilesDrawn = static_cast<int>(mBatches.size()) - mLastTilesStandIn;

    // Stand-ins label too, but only AFTER every real tile, and that order is
    // the whole trick. Without them the text blinks out for the frames a zoom
    // is in flight while the geometry underneath stays -- which is a worse
    // artefact than the blank map this was all meant to fix.
    //
    // Duplicates take care of themselves: a place named by both an ancestor and
    // the real tile lands at the SAME geographic point and so the same pixels,
    // and layOutText() rejects a candidate that collides with one already
    // placed. Real tiles going in first is what decides which of the two wins.
    for (std::size_t s = 0; s < mSources.size(); ++s)
    {
        for (std::size_t i = 0; i < mStandIns[s].size(); ++i)
        {
            if (mStandInTiles[s][i])
            {
                mLabelTiles.push_back(
                    map_render::LabelTile { mStandIns[s][i], mStandInTiles[s][i].labels });
            }
        }
    }

    // Fade bookkeeping stays bounded without ever re-fading the visible set:
    // prune only in bulk, only completed fades, and only when the table has
    // clearly outgrown any plausible viewport. See the member's comment.
    constexpr std::size_t kMaxFirstDrawn = 4096;
    if (mFirstDrawn.size() > kMaxFirstDrawn)
    {
        std::unordered_set<map_render::TileId, map_render::TileIdHash> onScreen;
        for (const map_render::GpuBatch& batch : mBatches)
        {
            onScreen.insert(batch.id);
        }
        std::erase_if(mFirstDrawn, [&](const auto& entry) {
            return !onScreen.contains(entry.first) && tileFadeAlpha(entry.first, now) >= 1.0F;
        });
    }
}

void MapWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    const QColor background = qt_helpers::toQColor(mConfig.style.background);

    if (mSources.empty() || width() <= 0 || height() <= 0)
    {
        mVisible.clear();
        mTileWalkTruncated = false;
        mLastTilesDrawn = 0;
        mLastTilesStandIn = 0;
        mLastLabelsPlaced = 0;
        painter.fillRect(rect(), background);
        return;
    }

    // The camera eases advance HERE, before the projection is built, so this
    // frame is drawn at the eased camera and tiles are fetched for it.
    mAnimating = tickAnimations(std::chrono::steady_clock::now());

    // Built here rather than only on resize: unlike resizeEvent this is
    // guaranteed to run before anything is drawn, at the size and the ratio
    // actually being painted.
    const map_render::Projection projection = projectionFor(painter);
    refreshTiles(projection);

    // --- 1. geometry, on the GPU -------------------------------------------

    assembleBatches();

    if (mGpu)
    {
        const map_render::GpuRenderer::Highlight highlight {
            mHighlightWayIds, qt_helpers::toQColor(mConfig.highlight_color),
            float(mConfig.highlight_extra_width)
        };
        // --- 1a. text, placed on the CPU and drawn with the frame ---------
        //
        // Placement and collision stay here -- which labels survive depends on
        // what else is already on screen, across tiles, so it cannot be baked
        // per tile. Only the DRAWING moves, and it was 95% of this pass.
        const map_render::LabelStats labels =
            map_render::layOutText(projection, mLabelTiles, mConfig.style, mLabelCache,
                                   projection.devicePixelRatio(), mTextQuads);
        mLastLabelsPlaced = labels.placed;
        mGpu->setText(mTextQuads, mLabelCache.atlas().page(), mLabelCache.atlas().dirty());
        mLabelCache.atlas().markClean();

        const QImage& frame =
            mGpu->render(projection, mBatches, mConfig.style, background, highlight);
        if (frame.isNull())
        {
            painter.fillRect(rect(), background);
        }
        else
        {
            // Straight blit, no scaling: the renderer was asked for exactly
            // this viewport, at exactly this screen's device pixel ratio, and
            // carries that ratio on the image. SmoothPixmapTransform would be
            // pure cost.
            painter.drawImage(QPointF(0.0, 0.0), frame);
        }
    }
    else
    {
        painter.fillRect(rect(), background);
    }

    // --- 2. the vehicle ----------------------------------------------------

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    paintMarker(painter, projection);

    if (mConfig.show_status)
    {
        paintDiagnostic(painter);
    }

    // The ONLY place the retry timer is armed, and it is enough: every drain
    // that changes a backoff also schedules a paint (failures repaint too,
    // above), and this paint may itself have retried tiles (in flight now,
    // their reply is the wake-up) or skipped some (off-viewport, dropped from
    // the schedule). One site, at the moment the request pass has just run,
    // cannot be caught with a stale view of what is due.
    armRetryTimer();

    // Never update() from inside a paint -- that is a repaint loop at event
    // rate. The ticker fires ~16 ms later and dies by itself once nothing is
    // fading.
    if ((mLastTilesFading > 0 || mAnimating) && !mAnimationTimer.isActive())
    {
        mAnimationTimer.start();
    }
}

void MapWidget::paintDiagnostic(QPainter& painter)
{
    // A map with nothing on it has several causes that look identical. Saying
    // which one it is here costs a line of text and saves the alternative,
    // which is reading app_logs to find out why a screenshot is empty.
    QString message;

    if (!mGpu)
    {
        message = QStringLiteral("No GPU backend — map geometry cannot be drawn");
    }
    else if (mLastTilesDrawn == 0 && mLastTilesStandIn == 0)
    {
        // Both, because this exists to explain a map with NOTHING on it. A
        // frame carrying stand-ins is a map -- older and coarser, but a map --
        // and captioning it "no coverage here" over the top of visible roads
        // is worse than saying nothing at all.
        // The BASE tileset's counters. An overlay that is absent is not what
        // makes the map empty -- the basemap is -- and captioning a blank
        // screen with the track archive's troubles would send somebody after
        // the wrong file.
        const map_widget::TileSourceStats sourceStats = mSources.front()->stats();

        if (sourceStats.requested == 0)
        {
            message = QStringLiteral("No tiles requested");
        }
        else if (sourceStats.decoded == 0 && sourceStats.failed > 0)
        {
            message = QStringLiteral("No reply from map_server on '%1'")
                          .arg(QString::fromStdString(mConfig.tile_zenoh_key));
        }
        else if (sourceStats.decoded == 0)
        {
            message = QStringLiteral("Waiting for tiles…");
        }
        else
        {
            message = QStringLiteral("No coverage here in tileset '%1'")
                          .arg(QString::fromStdString(mConfig.tileset));
        }
    }

    if (message.isEmpty())
    {
        return;
    }

    QFont font = painter.font();
    font.setPointSizeF(12.0);
    painter.setFont(font);
    painter.setPen(qt_helpers::toQColor(mConfig.style.label_text));
    painter.drawText(rect(), Qt::AlignCenter, message);
}

void MapWidget::paintMarker(QPainter& painter, const map_render::Projection& projection)
{
    if (!hasPosition())
    {
        return;
    }

    const QColor markerColor = qt_helpers::toQColor(mConfig.marker_color);

    if (mConfig.show_track && mTrack.size() >= 2)
    {
        QPainterPath path;
        bool started = false;
        for (const map_render::WorldPoint& point : mTrack)
        {
            const auto at = projection.screenFor(point);
            const QPointF pixel(at.x, at.y);
            if (!started)
            {
                path.moveTo(pixel);
                started = true;
            }
            else
            {
                path.lineTo(pixel);
            }
        }

        QPen pen(markerColor);
        pen.setWidthF(mConfig.track_width);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.setOpacity(mConfig.track_opacity);
        painter.drawPath(path);
        painter.setOpacity(1.0);
    }

    const auto at = projection.screenFor(map_render::Coordinate { *mLatitude, *mLongitude });
    const QPointF centre(at.x, at.y);
    const double radius = static_cast<double>(mConfig.marker_size);

    if (mHeading.has_value())
    {
        // A triangle pointing where the vehicle is going. When the map itself
        // rotates with heading the triangle points up the screen, which is
        // exactly right -- the marker's rotation is relative to the map, and
        // the map's is relative to north.
        const double screenHeading = *mHeading - projection.camera().bearing;
        const double radians = screenHeading * std::numbers::pi / 180.0;
        const double sin = std::sin(radians);
        const double cos = std::cos(radians);

        // Nose forward, two tails behind. In an unrotated frame "forward" is
        // -y, because screen y grows downward.
        const auto rotate = [&](double x, double y) {
            return QPointF(centre.x() + (x * cos) - (y * sin),
                           centre.y() + (x * sin) + (y * cos));
        };

        QPolygonF arrow;
        arrow << rotate(0.0, -radius * 1.4) << rotate(radius * 0.9, radius)
              << rotate(0.0, radius * 0.45) << rotate(-radius * 0.9, radius);

        painter.setPen(QPen(qt_helpers::toQColor(mConfig.marker_outline_color), 2.0));
        painter.setBrush(markerColor);
        painter.drawPolygon(arrow);
    }
    else
    {
        painter.setPen(QPen(qt_helpers::toQColor(mConfig.marker_outline_color), 2.0));
        painter.setBrush(markerColor);
        painter.drawEllipse(centre, radius, radius);
    }
}

void MapWidget::setHighlightWayIds(std::vector<std::uint64_t> ids)
{
    // The renderer's contract, enforced at the one door every caller uses:
    // sorted, so a tile's own sorted road list joins by walking, not searching.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids == mHighlightWayIds)
    {
        return;
    }
    mHighlightWayIds = std::move(ids);
    update();
}

void MapWidget::armRetryTimer()
{
    std::optional<std::chrono::steady_clock::time_point> due;
    for (const auto& source : mSources)
    {
        if (const auto at = source->nextRetryAt())
        {
            due = due ? std::min(*due, *at) : *at;
        }
    }
    if (!due)
    {
        mRetryTimer.stop();
        return;
    }

    // Ceil plus one so the paint lands AFTER retryAt: fire a hair early and
    // request() still sees the tile as waiting, skips it, and the whole
    // wake-up was for nothing.
    const auto wait =
        std::chrono::ceil<std::chrono::milliseconds>(*due - std::chrono::steady_clock::now());
    mRetryTimer.start(static_cast<int>(std::max<std::int64_t>(wait.count(), 0)) + 1);
}

namespace map_widget
{

std::vector<std::uint64_t> highlightWayIds(::MapHorizon::Reader horizon)
{
    std::vector<std::uint64_t> ids;
    if (!horizon.getHasPosition())
    {
        return ids;
    }
    ids.push_back(
        std::uint64_t(road_graph::wayOf(horizon.getPosition().getWhere().getSegmentId())));

    // The root path is first, by the schema's contract; its id scopes which
    // profiles are the road AHEAD rather than a side branch -- lighting every
    // branch would paint the whole junction.
    const auto paths = horizon.getPaths();
    if (paths.size() > 0)
    {
        const std::uint32_t rootId = paths[0].getPathId();
        for (const auto profile : horizon.getProfiles())
        {
            if (profile.getPathId() != rootId)
            {
                continue;
            }
            // Filtered, not switched: the schema grows new profile kinds
            // without breaking this consumer. The deliberate carve-out from
            // -Wswitch-enum, written into the schema comment.
            if (profile.getValue().which() != ::HorizonProfile::Value::SEGMENT)
            {
                continue;
            }
            ids.push_back(std::uint64_t(road_graph::wayOf(profile.getValue().getSegment())));
        }
    }

    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace map_widget

MapWidget::Status MapWidget::status() const
{
    Status out;
    if (!mSources.empty())
    {
        out.tiles = mSources.front()->stats();
        for (std::size_t s = 1; s < mSources.size(); ++s)
        {
            out.overlays.push_back(mSources[s]->stats());
        }
    }
    std::size_t visibleTotal = 0;
    for (const auto& ids : mVisible)
    {
        visibleTotal += ids.size();
    }
    out.tilesVisible = static_cast<int>(visibleTotal);
    out.tilesDrawn = mLastTilesDrawn;
    out.tilesStandIn = mLastTilesStandIn;
    out.tileWalkTruncated = mTileWalkTruncated;
    out.labelsPlaced = mLastLabelsPlaced;
    out.hasPosition = hasPosition();
    out.camera = camera();
    out.cameraMoved = mInteractionCentre.has_value();
    out.gpuReady = (mGpu != nullptr);
    out.retryPending = mRetryTimer.isActive();
    out.tilesFading = mLastTilesFading;
    out.animating = mZoomEase.has_value() || mRecentreEase.has_value();
    if (mGpu)
    {
        out.gpu = mGpu->stats();
    }
    return out;
}
