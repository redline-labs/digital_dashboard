// SPDX-License-Identifier: GPL-3.0-or-later
//
// An offline map, with the vehicle on it.
//
// Everything it draws comes from nodes/map_server over zenoh: no HTTP, and no
// off-the-shelf map widget. That last part is deliberate -- the Qt ones are
// QRhiWidget or QOpenGLWidget derived and do not initialise under
// QT_QPA_PLATFORM=offscreen, which would leave this invisible to ui_screenshot
// and to every `gui` test. See docs/map.md.
//
// Three passes, in this order, every paint:
//
//   1. The GPU frame. map_widget::GpuRenderer draws the tile geometry into an
//      offscreen texture through QRhi and hands back a QImage. Not a
//      QRhiWidget and not a QOpenGLWidget -- both bind to a platform surface
//      that does not exist offscreen. See map/gpu_renderer.h.
//
//      At the screen's DEVICE pixel ratio, not the logical size: passes 2 and 3
//      go through QPainter and are drawn at the ratio for free, so a logical
//      frame here put upscaled geometry under sharp text.
//
//      This is NOT free. The frame is close to linear in device pixels --
//      roughly 0.5 ms per megapixel on an M-series Metal backend, measured with
//      map_bench --dpr -- so a 660x640 widget pays about +0.6 ms to go to 2x
//      and a 2560x1440 one about +5.5 ms. It is the readback rather than the
//      rasterisation: turning MSAA off entirely moves 2x at 2560x1440 by less
//      than a millisecond. Worth it at dashboard sizes, and worth knowing about
//      before putting a full-screen map on a 4K panel at 60 Hz.
//   2. Labels, with QPainter. They must not rotate with the map and their
//      collision is viewport-global, so they cannot ride in the GPU transform
//      or be baked per tile. See map/labels.h.
//   3. The vehicle marker and its trail, also QPainter.
//
// There is no qt_helpers::CachedPaintWidget here and no cached underlay. That
// split exists to keep an expensive redraw off the hot path, and both expensive
// parts of this one are already cached somewhere better: tessellation happens
// once per tile on a zenoh thread inside TileSource, and a frame whose camera,
// tiles and style are unchanged is handed straight back out of GpuRenderer's
// own memo without being drawn or read back at all. A QPixmap here would be a
// third copy of the same picture.
#ifndef MAP_MAP_WIDGET_H
#define MAP_MAP_WIDGET_H

#include <atomic>
#include <deque>
#include <memory>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <string_view>
#include <vector>

#include <QTimer>
#include <QWidget>

namespace pub_sub
{
class RawSubscriber;
}

#include "dashboard/expression_subscription.h"
#include "dashboard/widget_types.h"

#include "map/config.h"
#include "map/gpu_renderer.h"
#include "map/labels.h"
#include "map/projection.h"
#include "map/recentre_button.h"
#include "map/tile_source.h"

class MapWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = MapConfig_t;
    static constexpr widget_type_t kWidgetType = widget_type_t::map;
    static constexpr std::string_view kFriendlyName = "Offline Map";

    explicit MapWidget(const config_t& config, QWidget* parent = nullptr);
    ~MapWidget() override;

    const config_t& getConfig() const { return mConfig; }

    // What the last paint actually drew, and what the tile source has done.
    //
    // Exposed because a blank map has several causes that look identical in a
    // screenshot -- no server running, no coverage at this location, a wrong
    // tileset name, a camera outside the archive's bounds, no GPU backend --
    // and this is what tells them apart without guessing.
    struct Status
    {
        // The BASE tileset's counters. Overlays report separately below rather
        // than being summed in: a working basemap with a missing track archive
        // and a missing basemap with working tracks are different faults, and
        // one set of totals cannot tell them apart.
        map_widget::TileSourceStats tiles;
        // One per entry in MapConfig_t::overlay_tilesets, in that order.
        std::vector<map_widget::TileSourceStats> overlays;
        int tilesVisible { 0 };
        int tilesDrawn { 0 };
        // Cached tiles from another zoom level drawn under the gaps while the
        // real ones are in flight. Non-zero during a zoom or a pan and back to
        // zero once the frame has settled; a number that STAYS non-zero means
        // tiles are not arriving.
        int tilesStandIn { 0 };
        // The visible-tile walk hit its cap and stopped early, so the map is
        // knowingly partial. Reachable by parking the camera several levels
        // shallower than the archive holds; without this the picture and a
        // genuine coverage hole look identical.
        bool tileWalkTruncated { false };
        int labelsPlaced { 0 };
        bool hasPosition { false };
        // False when no QRhi backend could be created. Hard failure: there is
        // no CPU fallback, so the map is background and labels only.
        bool gpuReady { false };
        map_widget::GpuRenderer::Stats gpu;
        // Where the map is ACTUALLY looking, which is not always the configured
        // centre: Follow Vehicle moves it, and so does a drag. A screenshot of
        // the wrong place and a screenshot of an archive with no coverage there
        // are the same picture, and this is what tells them apart.
        map_widget::Camera camera;
        // True once a drag has moved the camera off the configured centre or
        // off the vehicle. Also exactly what puts the recentre button on
        // screen -- see recentreCamera().
        bool cameraMoved { false };
        // True while the retry timer is armed against backed-off tiles. A map
        // that has settled -- served, failed-and-waiting, or empty -- shows
        // false; see armRetryTimer().
        bool retryPending { false };
        // Tiles drawn this frame that are still fading in. Non-zero means the
        // animation ticker is running and the GPU memo is deliberately
        // missing; stuck non-zero means a fade that never completes.
        int tilesFading { 0 };
        // True while a camera ease (wheel zoom or recentre fly-back) is in
        // flight. Stuck true means an ease that never lands.
        bool animating { false };
    };

    // GUI thread. Replace the set of OSM way ids drawn by the highlight
    // pass. This is the seam a route display drops into later: hand it the
    // route's way ids and the same pass draws them -- today the horizon
    // subscription below is the only caller. Sorted and deduplicated here,
    // which is the renderer's contract.
    void setHighlightWayIds(std::vector<std::uint64_t> ids);

    Status status() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    // All four no-op and pass to QWidget unless `interactive` is set, so a
    // dashboard that did not ask for a movable map behaves exactly as it did.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    void setLatitude(double degrees);
    void setLongitude(double degrees);
    void setHeading(double degrees);

    // Both coordinates seen at least once. Until then there is no position,
    // only half of one -- and acting on half puts the vehicle on the prime
    // meridian for a frame, a visible flick to the Gulf of Guinea and back on
    // every start.
    bool hasPosition() const { return mLatitude.has_value() && mLongitude.has_value(); }

    map_widget::Camera camera() const;
    // The camera, the logical viewport and the ratio of whatever is being
    // painted into, as one value. Built per paint rather than kept -- see
    // map/projection.h.
    map_widget::Projection projectionFor(const QPainter& painter) const;

    // The projection the MOUSE works against. Deliberately does not go looking
    // for a paint device -- there is none during an event -- and it does not
    // need one: every coordinate a Projection returns is logical, and the
    // device pixel ratio plays no part in screen<->world at all.
    map_widget::Projection interactionProjection() const;

    // Move the camera so that `world` ends up under `screen`. ONE primitive for
    // both gestures: a drag is "keep the point you grabbed under the pointer"
    // and a wheel is "keep the point under the pointer where it is while the
    // scale changes", which are the same sentence.
    void moveCameraSoThat(const map_widget::WorldPoint& world, const QPointF& screen);
    void setInteractionCentre(const map_widget::Coordinate& where);
    // `ease` is how long the zoom takes to glide to its target. A wheel's
    // detents are discrete and want the glide; a trackpad already sends a
    // smooth stream and passes zero, which lands the zoom on the next tick.
    // See wheelEvent().
    void zoomBy(double levels, const QPointF& at, std::chrono::milliseconds ease);
    // Drop the pan and go back to the vehicle, or to the configured centre when
    // there is no vehicle to follow. Does NOT touch the zoom: the user chose
    // that separately and asking to be recentred is not asking to be zoomed
    // back out.
    void recentreCamera();
    void layOutRecentreButton();

    void onPositionChanged();
    // Takes the frame's projection rather than making its own: the tile set has
    // to be the one the projection handed to the GPU is about to draw, and two
    // projections built independently is how those drift apart.
    void refreshTiles(const map_widget::Projection& projection);
    void paintMarker(QPainter& painter, const map_widget::Projection& projection);
    void paintDiagnostic(QPainter& painter);

    config_t mConfig;

    // The base tileset first, then each overlay in configured order.
    //
    // Separate sources rather than one archive with more layers in it, because
    // the two are updated on their own schedules from their own sources -- a
    // new drop of track maps must not mean rebuilding a 383 MB basemap, nor the
    // reverse. Each keeps its own zoom range, its own cache and its own
    // backoff, which is also what makes "no coverage here" and "that archive is
    // missing" distinguishable.
    std::vector<std::unique_ptr<map_widget::TileSource>> mSources;

    // Null when no backend came up. Checked on every paint rather than once,
    // because a null renderer is a state the widget draws differently rather
    // than a construction failure.
    std::unique_ptr<map_widget::GpuRenderer> mGpu;

    // Set on a zenoh thread, cleared on the GUI thread. It is what coalesces a
    // burst of tile replies into ONE repaint: the first arrival posts a queued
    // invoke and every later one sees the flag already set and posts nothing.
    //
    // Without the gate this is one QMetaCallEvent per tile, and a pan across a
    // city is a few hundred -- which is precisely how the CarPlay widget fell
    // behind before it was rewritten around a mailbox. With it, an idle map
    // costs nothing at all, which a 60 Hz poll would not.
    std::atomic<bool> mDrainPending { false };

    // Re-arm (or stop) mRetryTimer from the sources' backoff state. Called
    // after every drain and every paint -- the two moments backoff can change.
    void armRetryTimer();

    // The whole middle of the paint pass: what is ready, what stands in for
    // what is not, in the order the layer-major draw loop needs -- every
    // source's stand-ins first, then every real tile -- and each tile's fade.
    // Fills `batches` for the GPU and `labelTiles` for the label pass, and
    // updates the mLastTiles* counters.
    void assembleBatches();

    // This tile's crossfade at `now`: 0..1, smoothstepped, 1 when the fade is
    // done or disabled. First sight of a tile starts its clock.
    float tileFadeAlpha(const map_widget::TileId& id, std::chrono::steady_clock::time_point now);

    // Advance the camera eases to `now`, writing the eased values into the
    // same mInteractionZoom/mInteractionCentre optionals camera() already
    // reads -- the precedence rules are untouched. Returns true while an ease
    // is still in flight. Called at the top of paintEvent, so an ease
    // progresses exactly as fast as frames are drawn.
    bool tickAnimations(std::chrono::steady_clock::time_point now);

    // setInteractionCentre without the repaint or the button re-layout --
    // for callers already inside a paint.
    void setInteractionCentreQuiet(const map_widget::Coordinate& where);
    void moveCameraSoThatQuiet(const map_widget::WorldPoint& world, const QPointF& screen);

    // The one thing that repaints a failed map with nothing else going on: a
    // single-shot timer aimed at the earliest backed-off tile's retry time.
    // The paint it triggers is what re-issues the request. Never armed unless
    // something is backing off, so an idle healthy map still costs nothing.
    QTimer mRetryTimer;

    // The matched-road highlight, fed by nodes/map_match's horizon. Decoded
    // and reduced to way ids on the zenoh thread (see highlight_ids.h); the
    // GUI thread only swaps the result in. Its OWN coalescing flag, not the
    // tile gate's -- that handler drains tile sources and must not be
    // entangled with this one.
    std::unique_ptr<pub_sub::RawSubscriber> mHighlightSubscription;
    std::mutex mHighlightMutex;
    std::vector<std::uint64_t> mHighlightMailbox;
    bool mHighlightMailboxFresh { false };
    std::atomic<bool> mHighlightPending { false };
    // GUI thread only: what the paint pass hands the renderer.
    std::vector<std::uint64_t> mHighlightWayIds;

    // When each tile was FIRST drawn, which is when its fade started. Keyed by
    // id, not serial: a re-tessellation replaces the pixels, not the ground,
    // and must not dim the map. Entries are pruned only in bulk (see
    // paintEvent) so a tile that scrolls out and back does not fade again --
    // it is the same imagery, and a map that dims on every pan reads as
    // flicker, not polish.
    std::unordered_map<map_widget::TileId, std::chrono::steady_clock::time_point,
                       map_widget::TileIdHash>
        mFirstDrawn;

    // Scratch for the paint pass, reused across frames -- cleared each paint
    // with capacity kept, so a steady repaint allocates nothing. Written and
    // read only by assembleBatches()/paintEvent on the GUI thread.
    std::vector<map_widget::GpuBatch> mBatches;
    std::vector<map_widget::LabelTile> mLabelTiles;
    // The frame's text as quads. Reused rather than rebuilt, like every other
    // per-frame scratch here: the steady repaint allocates nothing.
    std::vector<map_widget::TextQuad> mTextQuads;
    std::vector<std::vector<map_widget::CachedTile>> mReady;
    std::vector<std::vector<float>> mAlphas;
    std::vector<std::vector<map_widget::TileId>> mStandIns;
    std::vector<std::vector<map_widget::CachedTile>> mStandInTiles;
    std::vector<bool> mHave;

    // The tile-walk memo. refreshTiles() runs on EVERY paint -- fades, eases
    // and label collision all repaint at a camera that has not moved -- and
    // the grid walk plus its centre-outward sort is pure recomputation then.
    // Keyed on exactly the inputs the walk reads: the camera (compared as
    // copies, not "close"), the viewport, and each source's archive range.
    // request() is still called every paint from the memoised lists: that is
    // what re-asks deferred tiles and expired backoffs, and it is already the
    // cheap early-out.
    std::optional<map_widget::Camera> mWalkCamera;
    int mWalkWidth { -1 };
    int mWalkHeight { -1 };
    std::vector<std::optional<map_widget::TileSource::ZoomRange>> mWalkRanges;
    std::vector<std::vector<map_widget::TileId>> mRequestLists;

    // Drives repaints while any tile is still fading or any camera ease is in
    // flight. Single-shot, re-armed at the end of each paint only while
    // something is animating -- an idle map keeps costing nothing.
    QTimer mAnimationTimer;
    int mLastTilesFading { 0 };
    bool mAnimating { false };

    // A wheel zoom in flight: the zoom glides from `from` to `to`, and when
    // `anchored` the grabbed world point is re-solved under the pointer at
    // every eased step -- the zoom-about-the-pointer property must hold
    // DURING the ease, not just at its ends. Not anchored while following
    // the vehicle: there the centre is never written and follow is never
    // broken, exactly as the instant zoom behaved.
    struct ZoomEase
    {
        double from { 0.0 };
        double to { 0.0 };
        map_widget::WorldPoint anchorWorld {};
        QPointF anchorScreen;
        bool anchored { false };
        std::chrono::steady_clock::time_point start;
        std::chrono::milliseconds length { 0 };
    };
    std::optional<ZoomEase> mZoomEase;

    // A recentre fly-back in flight: the centre glides from where the drag
    // left it toward the LIVE target -- re-read every tick, so a moving
    // vehicle is flown TO, not to where it was when the button was pressed.
    // While it flies, mInteractionCentre stays set (that is what "following
    // suspended" means); landing resets it and normal follow resumes.
    struct RecentreEase
    {
        map_widget::Coordinate from {};
        std::chrono::steady_clock::time_point start;
    };
    std::optional<RecentreEase> mRecentreEase;

    // Parallel to mSources: each picks its own integer zoom from its own
    // archive's range, so a global track layer that stops at z14 and a regional
    // basemap that goes deeper are asked for different levels in the same
    // frame.
    std::vector<std::vector<map_widget::TileId>> mVisible;

    std::optional<double> mLatitude;
    std::optional<double> mLongitude;
    std::optional<double> mHeading;

    // The camera the USER put the map on, which beats both the configured
    // centre and the vehicle. Two separate optionals rather than one Camera,
    // because they are suspended independently and only one of them breaks
    // Follow Vehicle:
    //
    //   * A drag sets the centre, and having a centre IS what "following is
    //     suspended" means. There is no second flag to keep in step with it.
    //   * The wheel sets only the zoom. Zooming while following the vehicle
    //     zooms about the CENTRE rather than the pointer, so the vehicle does
    //     not move on screen and there is no reason to stop tracking it --
    //     wanting a closer look is not asking to be left behind.
    std::optional<map_widget::Coordinate> mInteractionCentre;
    std::optional<double> mInteractionZoom;

    // The world point grabbed on mouse-down, held for the whole drag. Fixed at
    // press rather than recomputed per move: chasing a delta between successive
    // move events accumulates the rounding of every one of them, and the map
    // slides out from under the pointer over a long drag.
    std::optional<map_widget::WorldPoint> mDragAnchor;

    // Null unless the map is interactive. Parented to this widget, so the
    // editor's recursive mouse-transparency reaches it in edit mode.
    map_widget::RecentreButton* mRecentre { nullptr };

    // WORLD points, not coordinates. A track point's world position is fixed
    // the moment it arrives; only the camera moves. Keeping degrees meant
    // re-running the Mercator forward transform over the whole trail on every
    // paint to compute a number that had not changed.
    std::deque<map_widget::WorldPoint> mTrack;

    // Glyph outlines for the label pass, kept between frames. See map/labels.h.
    map_widget::LabelCache mLabelCache;

    // Written by paintEvent, read by status(). Both on the GUI thread.
    int mLastTilesDrawn { 0 };
    int mLastTilesStandIn { 0 };
    bool mTileWalkTruncated { false };
    int mLastLabelsPlaced { 0 };

    dashboard::ExpressionSubscriptionPtr<double> mLatitudeSubscription;
    dashboard::ExpressionSubscriptionPtr<double> mLongitudeSubscription;
    dashboard::ExpressionSubscriptionPtr<double> mHeadingSubscription;
};

#endif // MAP_MAP_WIDGET_H
