#ifndef SCOPE_TIME_SERIES_PANEL_H_
#define SCOPE_TIME_SERIES_PANEL_H_

#include "scope/decimate.h"
#include "scope/panel.h"
#include "scope/panel_types.h"
#include "scope/sample_ring.h"
#include "time_series/config.h"

#include <QColor>
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
    static constexpr panel_type_t kPanelType = panel_type_t::time_series;
    static constexpr std::string_view kFriendlyName = "Time Series";

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

    // Per-signal buffer state, for the agent interface's sample_stats.
    struct SignalStats
    {
        std::string label;
        std::size_t retained = 0;
        std::uint64_t received = 0;
        std::uint64_t dropped = 0;
        bool bound = false;
        double t_first = 0.0;
        double t_last = 0.0;
        double min = 0.0;
        double max = 0.0;
        double last = 0.0;
        bool has_data = false;
    };
    std::vector<SignalStats> stats() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Trace;

    void rebindAll();
    void releaseAll();
    void onFrame();

    // The rectangle the traces live in: the widget minus the axis gutters.
    QRectF plotRect() const;

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
};

}  // namespace scope

#endif  // SCOPE_TIME_SERIES_PANEL_H_
