#ifndef SCOPE_TIME_SERIES_PANEL_H_
#define SCOPE_TIME_SERIES_PANEL_H_

#include "scope/decimate.h"
#include "scope/panel.h"
#include "scope/panel_types.h"
#include "scope/sample_ring.h"
#include "scope/time_axis.h"
#include "time_series/config.h"
#include "time_series/stats.h"

#include <QColor>
#include <QPoint>
#include <QRectF>
#include <QString>

#include <memory>
#include <vector>

namespace scope
{

class DataSource;
class TimeBase;

// A plot of several signals against a shared time axis.
//
// Traces are reduced to one min/max span per pixel column (see decimate.h), so
// the per-frame cost is bounded by the widget's width rather than by how much
// history is retained. That is what makes this cheap, and it is the only thing
// that needs to be.
//
// It does NOT use qt_helpers::CachedPaintWidget, which is the tree's usual way
// to keep a repaint cheap, and the reason is worth stating so nobody adds it
// back. That helper caches a "static" underlay in a pixmap. Here the underlay
// is the grid and its axis labels -- and with autoscale on, which is the
// default, the vertical range changes whenever the visible data does, so the
// grid would be re-rendered almost every frame anyway. Paying a pixmap
// allocation and a blit for a layer that is invalid most of the time is worse
// than drawing it. A panel is also not free to derive from it in any case:
// Panel is already the QWidget base, and CachedPaintWidget is another one.
//
// If a plot with autoscale off ever shows up hot in a profile, caching the grid
// behind an "is the Y range unchanged" check is the thing to do -- not adopting
// a base class that assumes the layer is static.
class TimeSeriesPanel : public Panel
{
    Q_OBJECT

  public:
    using config_t = TimeSeriesPanelConfig_t;
    using stats_t = TimeSeriesPanelStats_t;
    static constexpr panel_type_t kPanelType = panel_type_t::time_series;
    static constexpr std::string_view kFriendlyName = "Time Series";
    // U+223F SINE WAVE. Picked over the more obvious chart glyphs because it is
    // in the default font on every platform this runs on -- U+2382 and the
    // emoji chart symbols are not, and a missing glyph renders as a blank box
    // that looks like a broken button rather than a plain one.
    static constexpr std::string_view kToolbarGlyph = "∿";

    // Retention, when the window does not say otherwise. Generous on time
    // because scrolling back is the point of a scope; the workspace's
    // `history_seconds` overrides it.
    static constexpr double kDefaultHistorySeconds = 300.0;

    // `source` must outlive the panel: it owns the subscriptions this binds.
    TimeSeriesPanel(const config_t& cfg, DataSource& source,
                    double history_seconds = kDefaultHistorySeconds,
                    QWidget* parent = nullptr);
    ~TimeSeriesPanel() override;

    panel_type_t panelType() const override { return kPanelType; }
    bool acceptsBinding(const BindingCandidate& candidate) const override;
    bool addBinding(const BindingCandidate& candidate) override;
    void setTimeBase(TimeBase* time_base) override;
    void setHistorySeconds(double seconds) override;
    void rebindTo(DataSource& source) override;
    QString title() const override;

    double historySeconds() const { return history_seconds_; }

    const config_t& getConfig() const { return cfg_; }

    // Replaces the whole configuration, rebinding whatever changed. Signals
    // that are unchanged keep their history rather than being torn down and
    // restarted -- the same reasoning as the editor's undo diffing, where
    // rebuilding an untouched widget threw away work for nothing.
    void applyConfig(const config_t& cfg);

    bool removeSignal(std::size_t index);

    // What this panel actually received. Harvested generically by
    // panelStatsOf() -- see panel_registry.h -- so the agent interface serves it
    // without knowing this type exists.
    stats_t stats() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

    // Navigation. Every one of these moves the SHARED TimeBase rather than
    // anything of this panel's own, which is what makes the other panels follow
    // -- a plot is a view onto one window, not a window of its own.
    //
    // The Y gestures are the exception and are deliberately per-panel: two
    // signals three orders of magnitude apart share a time axis and cannot share
    // a value axis, which is the same reason `right_axis` exists.
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

  private:
    struct Trace;

    void rebindAll();
    void releaseAll();
    void onFrame();

    // The rectangle the traces live in: the widget minus the axis gutters.
    QRectF plotRect() const;

    // The time-to-pixel map for what was drawn LAST FRAME, which is what every
    // gesture converts against. Built from drawn_begin_/drawn_end_ rather than
    // from the time base, so the instant under the pointer is the one the user
    // can actually see -- during live scrolling those differ by a frame, which
    // is enough to put a click on the wrong sample at a tight zoom.
    TimeAxis timeAxis() const;

    // Zoom the value axis about a pixel, turning autoscale off. Per-panel, and
    // it writes the config, so the workspace is genuinely dirty afterwards.
    void zoomValueAxis(double at_y, double factor);

    // A drag has to travel this far before it becomes a pan, or every click
    // would nudge the shared window. Below the threshold press-and-release does
    // nothing at all, which is what lets a click-to-focus stay harmless.
    bool dragExceededThreshold(const QPoint& pos) const;

    // Vertical extent to draw against, from the config or from the data.
    void computeYRange(double& y_min, double& y_max, bool right_axis) const;

    void paintGrid(QPainter& painter, const QRectF& area, double y_min, double y_max);
    void paintTraces(QPainter& painter, const QRectF& area);
    void paintLegend(QPainter& painter);
    void paintCursor(QPainter& painter, const QRectF& area);

    config_t cfg_;

    // A pointer, not a reference: rebindTo() moves every trace onto a different
    // source when the window enters review over a recording.
    DataSource* source_;

    TimeBase* time_base_ = nullptr;
    double history_seconds_ = kDefaultHistorySeconds;

    std::vector<std::unique_ptr<Trace>> traces_;

    // Kept across frames so a 30 Hz redraw allocates nothing.
    mutable std::vector<ColumnStats> columns_;

    // The window actually drawn last frame, so the cursor readout and the
    // hover-to-time conversion agree with what is on screen.
    double drawn_begin_ = 0.0;
    double drawn_end_ = 0.0;
    double drawn_y_min_ = 0.0;
    double drawn_y_max_ = 1.0;

    // The right-hand axis, scaled independently. Two signals whose ranges
    // differ by three orders of magnitude -- engine rpm and oil pressure, say
    // -- are unreadable on one scale: the smaller is a flat line along the
    // bottom. Assigning one to the right axis is how you compare their shapes.
    double drawn_y2_min_ = 0.0;
    double drawn_y2_max_ = 1.0;
    bool has_right_axis_ = false;

    // ------------------------------------------------------------- gestures

    enum class Drag
    {
        None,

        // The button is down but has not travelled far enough to mean anything
        // yet. A click that never becomes a drag leaves the view untouched.
        Pending,

        // Sliding the shared window.
        Pan,

        // Rubber-banding a time range to zoom to.
        Band,
    };

    Drag drag_ = Drag::None;
    QPoint drag_origin_;

    // The pointer's position at the last move, in pixels. A pan is applied from
    // the DELTA rather than from the origin because the window moves underneath
    // as we go: measuring against the origin would apply the same offset again
    // every frame and the plot would accelerate away.
    QPoint drag_last_;

    // The band's edges while one is being dragged, on the panel's clock.
    double band_begin_ = 0.0;
    double band_end_ = 0.0;
};

}  // namespace scope

#endif  // SCOPE_TIME_SERIES_PANEL_H_
