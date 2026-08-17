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
#include <optional>
#include <string_view>
#include <vector>

#include <QWidget>

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
        map_widget::TileSourceStats tiles;
        int tilesVisible { 0 };
        int tilesDrawn { 0 };
        // Cached tiles from another zoom level drawn under the gaps while the
        // real ones are in flight. Non-zero during a zoom or a pan and back to
        // zero once the frame has settled; a number that STAYS non-zero means
        // tiles are not arriving.
        int tilesStandIn { 0 };
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
    };

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
    void zoomBy(double levels, const QPointF& at);
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

    std::unique_ptr<map_widget::TileSource> mTiles;

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

    std::vector<map_widget::TileId> mVisible;

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
    int mLastLabelsPlaced { 0 };

    dashboard::ExpressionSubscriptionPtr<double> mLatitudeSubscription;
    dashboard::ExpressionSubscriptionPtr<double> mLongitudeSubscription;
    dashboard::ExpressionSubscriptionPtr<double> mHeadingSubscription;
};

#endif // MAP_MAP_WIDGET_H
