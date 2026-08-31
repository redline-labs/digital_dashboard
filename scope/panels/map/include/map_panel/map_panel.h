#ifndef SCOPE_MAP_PANEL_H_
#define SCOPE_MAP_PANEL_H_

// For SignalHandle, which the Signal struct below holds by value. TablePanel
// keeps its equivalent in the .cpp and forward-declares DataSource; this one
// cannot, because the three roles are fixed members rather than a list.
#include "scope/data_source.h"
#include "scope/panel.h"
#include "scope/panel_types.h"
#include "scope/sample_ring.h"
#include "scope/settings.h"

#include "map_panel/config.h"
#include "map_controls/compass_button.h"
#include "map_controls/recentre_button.h"
#include "map_controls/view_mode_button.h"
#include "map_controls/zoom_button.h"
#include "map_panel/stats.h"
#include "map_panel/tile_reader.h"
#include "map_panel/track_builder.h"

#include "map_render/map_pass.h"
#include "map_surface/map_surface.h"
#include "map_render/labels.h"
#include "map_render/projection.h"

#include <QString>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace scope
{

class DataSource;
class TimeBase;

// Where the vehicle went, under the shared clock.
//
// The fourth panel. It draws the whole retention window as a trail, puts a
// marker on the shared cursor, and -- the part no other panel does -- lets a
// click on the trail MOVE that cursor, so every plot, table and video frame
// jumps to the corner you pointed at. A plot answers "what was the speed at
// this moment"; this answers "where on the lap was I slow", which is the
// question a plot is worst at.
//
// A SEPARATE IMPLEMENTATION FROM dashboard/widgets/map, deliberately, the same
// call scope/panels/video/video_decoder made against CarPlayWidget's decoder.
// The two answer different questions: that one follows a live vehicle and pulls
// tiles from nodes/map_server over zenoh; this one reviews a recording and
// reads an .mbtiles off disk. What IS shared is everything that turns a vector
// tile into pixels -- projection, tessellator, GPU renderer, label layout, tile
// cache -- which is libs/map_render, hoisted out of the widget for exactly this.
//
// THE PANEL DOES NOT DRAW THE BASEMAP. It drives a map_surface::MapSurface
// child, which picks between drawing straight into a QRhiWidget and going
// through an offscreen texture depending on what the platform can do -- and
// which one it got is not visible from here. See map_surface/map_surface.h.
//
// Two consequences that ARE this panel's business:
//
//   * The trail, the marker, the legend and the diagnostic are painted through
//     setOverlayPainter(), not in this panel's own paintEvent. A QRhiWidget has
//     no paint engine, so QPainter over the map only works on the transparent
//     layer the surface puts there.
//   * paintEvent stays the frame driver, and it still paints the panel's
//     background. Driving from there is what keeps render() and repaint()
//     meaning "produce one frame" for every gui test and every ui_screenshot.
//
// No CachedPaintWidget -- a Panel cannot derive from both bases anyway, and the
// cached layer would invalidate on every cursor move.
class MapPanel : public Panel
{
    Q_OBJECT

  public:
    using config_t = MapPanelConfig_t;
    using stats_t = MapPanelStats_t;

    static constexpr panel_type_t kPanelType = panel_type_t::map;
    static constexpr std::string_view kFriendlyName = "Map";

    // U+25C9 FISHEYE -- a marker on a ground. Same rule as the other three
    // glyphs: present in the default font on every platform this runs on, which
    // the emoji alternatives are not, and distinct at a glance from ▤ ▶ and the
    // plot's own.
    static constexpr std::string_view kToolbarGlyph = "◉";

    // A trail wants more history than a readout and about as much as a plot: it
    // IS the retention window, drawn. The workspace's history_seconds overrides
    // it, and raising that is how you see a whole lap at once.
    static constexpr double kDefaultHistorySeconds = 300.0;

    // `source` must outlive the panel: it owns the subscriptions this binds.
    MapPanel(const config_t& cfg, DataSource& source,
             double history_seconds = kDefaultHistorySeconds, QWidget* parent = nullptr);
    ~MapPanel() override;

    panel_type_t panelType() const override { return kPanelType; }
    bool acceptsBinding(const BindingCandidate& candidate) const override;
    bool addBinding(const BindingCandidate& candidate) override;
    std::vector<QString> bindingLabels() const override;
    std::size_t unboundBindingCount() const override;
    bool removeBinding(std::size_t index) override;
    void setTimeBase(TimeBase* time_base) override;
    void setHistorySeconds(double seconds) override;
    void rebindTo(DataSource& source) override;
    QString title() const override;

    const config_t& getConfig() const { return cfg_; }
    void applyConfig(const config_t& cfg);

    // The session's mode toggles -- what the compass and view buttons flip.
    // Public because a control (or a test) flips them; the CONFIG's fields are
    // what the workspace opens with, and these never write back to it.
    // A compass CLICK straightens first: a manually spun map snaps back to
    // the configured bearing, and only a click on an unspun map cycles the
    // orientation. Dragging the needle spins the map (setManualBearing).
    void cycleOrientation();
    void toggleViewMode();
    void setManualBearing(double degrees);
    // One zoom step about the viewport centre -- the buttons' path. Same
    // semantics as the wheel, including breaking Follow Cursor.
    void zoomStep(double levels);
    MapPanelOrientation_t effectiveOrientation() const
    {
        return orientation_override_.value_or(cfg_.orientation);
    }
    MapViewMode_t effectiveViewMode() const
    {
        return view_override_.value_or(cfg_.view_mode);
    }
    // Drop the pan and the wheel zoom and go back to Follow Cursor -- the way
    // back that the panel never had; see stats().camera_moved.
    void recentreCamera();
    stats_t stats() const;

    // Where the archives are, by the name cfg_.tileset refers to them by.
    //
    // Pushed in rather than read from disk here, so a headless run and a test
    // drive the panel through one path and never touch the developer's real
    // settings file. Re-opens the readers, because the answer to "where is
    // 'socal'" has just changed underneath them.
    void setSettings(const scope_settings_t& settings);

    double historySeconds() const { return history_seconds_; }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    // THE GESTURE ARBITRATION, and it is the whole interaction model: a press
    // landing on the drawn track scrubs the SHARED time base; a press anywhere
    // else pans the map. The wheel always zooms.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    // Which role a binding fills. The panel takes three signals rather than a
    // list, so "which one is this" is a fixed question with a fixed answer --
    // unlike a plot, where every trace is the same kind of thing.
    enum class Role
    {
        Latitude,
        Longitude,
        Color,
    };

    struct Signal
    {
        map_binding_t binding;
        std::shared_ptr<SignalBuffer> buffer;
        SignalHandle handle = kInvalidSignal;
        bool bound = false;
    };

    void bindRole(Role role, const map_binding_t& binding);
    void rebindAll();
    void releaseAll();
    const map_binding_t& bindingFor(Role role) const;
    map_binding_t& bindingFor(Role role);
    Signal& signalFor(Role role);
    const Signal& signalFor(Role role) const;

    // Open a TileReader per configured tileset, base first then overlays.
    // Called on construction, on a config change that names different tilesets,
    // and on setSettings().
    void openReaders();

    void onFrame();

    // THE INSTANT THE MARKER IS DRAWN FOR. The shared cursor when there is one,
    // otherwise the view's right edge -- which is the source's clock live and
    // the playhead on a recording. Identical to TablePanel::readoutTime(), and
    // it must stay identical or the marker and the table row disagree about the
    // same moment.
    double readoutTime() const;
    bool readingAtCursor() const;

    // Rebuild track_ from the bound buffers. Called once per frame rather than
    // per paint, because a paint can happen for reasons that did not change the
    // data -- an expose, a tile arriving.
    void rebuildTrack();

    // Where the camera actually is, in precedence order: a manual drag, then
    // the marker if Follow Cursor is on, then the configured centre.
    map_render::Camera camera() const;

    // Which tiles the camera wants, and asking the readers for them.
    void refreshTiles(const map_render::Projection& projection);
    void assembleBatches();

    // Everything this panel draws over the basemap, on the surface's
    // transparent layer. Hung on the surface once, in the constructor.
    void paintOverlay(QPainter& painter);
    void paintTrack(QPainter& painter, const map_render::Projection& projection);
    void paintMarker(QPainter& painter, const map_render::Projection& projection);
    void paintLegend(QPainter& painter);
    void paintDiagnostic(QPainter& painter);

    // The caption explaining an empty map, or empty when nothing is wrong.
    // ONE function, so what the panel paints and what scope.stats reports can
    // never disagree -- every cause below looks identical in a screenshot.
    QString diagnostic() const;

    QColor colorForValue(double value) const;
    // The range the ramp spans: the data's own when autoscaling, else the
    // configured pair. Returns false when there is nothing to scale. O(1): the
    // autoscale extremes are cached by rebuildTrack(), because this is called
    // once per drawn SEGMENT and a scan of track_ here made paintTrack
    // O(segments x points) -- millions of point visits per frame to recompute a
    // constant.
    bool colorRange(double& low, double& high) const;

    config_t cfg_;

    // A pointer, not a reference: rebindTo() moves the signals onto a different
    // source when the window opens a recording.
    DataSource* source_;

    TimeBase* time_base_ = nullptr;
    double history_seconds_ = kDefaultHistorySeconds;

    Signal latitude_;
    Signal longitude_;
    Signal color_;

    scope_settings_t settings_;

    // Base first, then overlays, in config order -- which is the order they are
    // drawn in.
    std::vector<std::unique_ptr<TileReader>> readers_;
    // The name each reader was opened for, so a diagnostic can say which.
    std::vector<std::string> reader_names_;
    // A tileset named in the config that Settings does not know. Kept so the
    // caption can name it: "not configured" and "configured but unreadable" are
    // different fixes.
    std::vector<std::string> unknown_tilesets_;

    // The basemap, filling this panel and beneath everything else in it. Owned
    // by Qt as a child, never null: it reports its own failure through
    // isUsable(), which is a state the panel REPORTS rather than one it dies
    // of -- the trail and the marker still draw over an empty map.
    map_surface::MapSurface* surface_ { nullptr };
    // What the last submitted frame was drawn for. The overlay paints after
    // paintEvent has returned, so the projection it needs cannot be a local.
    std::optional<map_render::Projection> frame_projection_;
    map_render::LabelCache label_cache_;

    // The trail, rebuilt each frame from the bound buffers, and the thinned
    // form the path and the hit test both use. Members rather than locals so a
    // 30 Hz rebuild does not allocate.
    std::vector<track::Point> track_;
    std::vector<track::Point> thinned_;
    track::BuildStats track_stats_;

    // The autoscale extremes of track_'s colour values, computed once per
    // rebuildTrack(). false when no point carries a colour.
    bool track_color_valid_ = false;
    double track_color_low_ = 0.0;
    double track_color_high_ = 1.0;

    // What the track was last built from and where the shared instant last
    // was, so onFrame() can skip the rebuild and the repaint when neither
    // moved. See onFrame().
    std::uint64_t track_signature_ = 0;
    double last_frame_readout_ = 0.0;

    // Paint scratch, kept across frames for the same reason.
    std::vector<std::vector<map_render::TileId>> visible_;
    std::vector<std::vector<map_render::TileId>> request_lists_;
    std::vector<std::vector<map_render::CachedTile>> ready_;
    std::vector<std::vector<map_render::TileId>> stand_ins_;
    std::vector<std::vector<map_render::CachedTile>> stand_in_tiles_;
    std::vector<map_render::GpuBatch> batches_;
    std::vector<map_render::LabelTile> label_tiles_;
    std::vector<map_render::TextQuad> text_quads_;

    // Where a drag put the camera, if anywhere. Empty means Follow Cursor or
    // the configured centre decides -- see camera().
    std::optional<map_render::Coordinate> drag_centre_;
    std::optional<double> drag_zoom_;

    // Mid-gesture state. A press either grabs the TRACK (scrubbing the shared
    // time base) or the MAP (panning), decided once at press time and not
    // revisited, so a drag cannot change its mind halfway.
    enum class Gesture
    {
        None,
        Panning,
        Scrubbing,
    };
    Gesture gesture_ = Gesture::None;
    map_render::ScreenPoint press_screen_;
    map_render::WorldPoint press_world_;

    // What the last frame drew, for stats(). Counted rather than recomputed:
    // stats() must describe the frame on screen, not a hypothetical one.
    int last_tiles_drawn_ = 0;
    int last_tiles_stand_in_ = 0;
    bool marker_valid_ = false;
    double marker_t_ = 0.0;
    map_render::Coordinate marker_coordinate_;

    // Course over ground at the marker, smoothed over its neighbours --
    // computed beside the marker each paint, consumed by camera() when the
    // orientation is course_up. Empty when the track is too short to say.
    std::optional<double> course_deg_;

    // The mode toggles, mirroring drag_centre_/drag_zoom_ above: empty means
    // "as configured", set means a button was pressed this session. Never
    // written to cfg_ -- a view button must not edit the workspace.
    std::optional<MapPanelOrientation_t> orientation_override_;
    std::optional<MapViewMode_t> view_override_;
    // The compass drag's spin; beats the configured bearing while set,
    // cleared by a compass click. A drag forces north_up on its way in.
    std::optional<double> bearing_override_;

    // Null unless the panel is interactive. Corner chrome, top-right --
    // paintLegend owns the bottom-right.
    map_controls::RecentreButton* recentre_ = nullptr;
    map_controls::CompassButton* compass_ = nullptr;
    map_controls::ViewModeButton* view_mode_ = nullptr;
    map_controls::ZoomButton* zoom_in_ = nullptr;
    map_controls::ZoomButton* zoom_out_ = nullptr;
    void layOutMapButtons();
};

}  // namespace scope

#endif  // SCOPE_MAP_PANEL_H_
