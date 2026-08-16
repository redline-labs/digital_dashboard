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
//   2. Labels, with QPainter. They must not rotate with the map and their
//      collision is viewport-global, so they cannot ride in the GPU transform
//      or be baked per tile. See map/labels.h.
//   3. The vehicle marker and its trail, also QPainter.
//
// There is no qt_helpers::CachedPaintWidget here and no cached underlay. That
// split exists to keep an expensive redraw off the hot path, and on this widget
// the expensive part is tessellation -- which TileSource already does once per
// tile on a zenoh thread. What is left is a ~0.2 ms GPU frame that is flat in
// resolution, so caching it would cost a full-size QPixmap to save nothing.
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
#include "map/projection.h"
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
        int labelsPlaced { 0 };
        bool hasPosition { false };
        // False when no QRhi backend could be created. Hard failure: there is
        // no CPU fallback, so the map is background and labels only.
        bool gpuReady { false };
        map_widget::GpuRenderer::Stats gpu;
    };

    Status status() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

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
    void onPositionChanged();
    void refreshTiles();
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

    // WORLD points, not coordinates. A track point's world position is fixed
    // the moment it arrives; only the camera moves. Keeping degrees meant
    // re-running the Mercator forward transform over the whole trail on every
    // paint to compute a number that had not changed.
    std::deque<map_widget::WorldPoint> mTrack;

    // Written by paintEvent, read by status(). Both on the GUI thread.
    int mLastTilesDrawn { 0 };
    int mLastLabelsPlaced { 0 };

    dashboard::ExpressionSubscriptionPtr<double> mLatitudeSubscription;
    dashboard::ExpressionSubscriptionPtr<double> mLongitudeSubscription;
    dashboard::ExpressionSubscriptionPtr<double> mHeadingSubscription;
};

#endif // MAP_MAP_WIDGET_H
