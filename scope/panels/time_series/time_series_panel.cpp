#include "time_series/time_series_panel.h"

#include "scope/bound_signal_list.h"
#include "scope/data_source.h"
#include "scope/state_names.h"
#include "scope/time_base.h"
#include "scope/value_format.h"

#include "qt_helpers/widget_colors.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace scope
{

namespace
{

// Bounded on points so a fast publisher cannot grow a session without bound.
// The time bound is the workspace's `history_seconds`, which arrives through the
// constructor; 5 minutes at 1 kHz is 300k points, well inside this cap.
constexpr std::size_t kMaxPointsPerSignal = 600000;

// One GUI tick at 30 Hz is 33 ms; 4096 slots is over a second of headroom at
// 1 kHz. Overflow past this means the GUI thread is wedged, and then nothing is
// being drawn anyway.
constexpr std::size_t kStagingCapacity = 4096;

// Axis gutters, in logical pixels.
constexpr double kLeftGutter = 56.0;
constexpr double kRightGutter = 12.0;
constexpr double kRightAxisGutter = 56.0;
constexpr double kTopGutter = 8.0;
constexpr double kBottomGutter = 22.0;

constexpr int kTargetYTicks = 5;
constexpr int kTargetXTicks = 6;

// A "nice" step at or just above `rough`: 1, 2, 5 or 10 times a power of ten.
// Without this the grid lands on values like 0.037, which is unreadable and
// makes two panels side by side impossible to compare.
double niceStep(double rough)
{
    if (!(rough > 0.0) || !std::isfinite(rough))
    {
        return 1.0;
    }
    const double magnitude = std::pow(10.0, std::floor(std::log10(rough)));
    const double normalized = rough / magnitude;
    if (normalized <= 1.0)
    {
        return magnitude;
    }
    if (normalized <= 2.0)
    {
        return 2.0 * magnitude;
    }
    if (normalized <= 5.0)
    {
        return 5.0 * magnitude;
    }
    return 10.0 * magnitude;
}

// The fixed palette, parsed once. QColor("#RRGGBB") goes through a string
// parser; a dozen of these per paintEvent at 30 Hz is pure waste.
const QColor kBackground("#14161A");
const QColor kFrame("#3A4048");
const QColor kForeground("#E0E4EA");
const QColor kGridLine("#232830");
const QColor kGridLabel("#8A94A6");
const QColor kLaneRowFill("#181B20");
const QColor kLaneTextDark("#0B0D10");
const QColor kLegendMuted("#5A6270");
const QColor kLegendBound("#C8CEDA");
const QColor kLegendUnbound("#8A6060");
const QColor kDefaultTrace("#4FC3F7");

// Height of one state lane, and the gap between the plot and the first of them.
constexpr double kLaneHeight = 20.0;
constexpr double kLaneGap = 6.0;

// Colours for state bands, indexed by ordinal. Distinct rather than a ramp:
// these are labels, not quantities, so a gradient would imply an ordering that
// an enum does not have. Wraps for an enum with more enumerants than this, which
// is why the band also carries its NAME.
const QColor& stateColor(std::size_t ordinal)
{
    static const std::vector<QColor> palette = {
        QColor("#2E5A6E"), QColor("#4FC3F7"), QColor("#7E57C2"), QColor("#66BB6A"),
        QColor("#FFA726"), QColor("#EF5350"), QColor("#26A69A"), QColor("#AB47BC"),
    };
    return palette[ordinal % palette.size()];
}

}  // namespace

// One plotted signal: its configuration, where its samples land, and the handle
// that keeps it bound.
struct TimeSeriesPanel::Trace
{
    signal_binding_t binding;
    std::shared_ptr<SignalBuffer> buffer;
    SignalHandle handle = kInvalidSignal;
    QColor color;

    // Drawn as a state lane rather than as a line. Resolved at bind time from
    // the config's `display` and, when that is `automatic`, from what the field
    // turned out to be.
    bool lane = false;

    // What this binding's states are called, resolved from the schema at bind
    // time. Empty names for a state channel whose numbers have none (a forced
    // lane over a plain integer): a band with no name still shows its number,
    // which is strictly better than a sloping line.
    StateNames states;

    // False when binding failed -- a bad expression, a non-numeric field, a
    // subscription that would not declare. The legend says so rather than
    // showing an empty trace that looks like a quiet signal.
    bool bound = false;

    // This trace's per-column reduction of the drawn window, refreshed once per
    // paint and kept across frames so a 30 Hz redraw allocates nothing. The
    // autoscale is derived from these SAME columns -- the column partition
    // covers the window exactly, so min over columns equals min over samples --
    // which is what lets one pass serve both instead of the two full scans this
    // used to cost.
    std::vector<ColumnStats> columns;
    std::size_t filled = 0;

    QString displayLabel() const
    {
        if (!binding.label.empty())
        {
            return QString::fromStdString(binding.label);
        }
        return QString::fromStdString(binding.value_expression);
    }
};

TimeSeriesPanel::TimeSeriesPanel(const config_t& cfg, DataSource& source, double history_seconds,
                                 QWidget* parent) :
    Panel(parent), cfg_(cfg), source_(&source), history_seconds_(history_seconds)
{
    setMouseTracking(true);  // For the hover cursor, which needs no button held.
    setMinimumSize(160, 90);
    setAutoFillBackground(false);
    rebindAll();
}

TimeSeriesPanel::~TimeSeriesPanel()
{
    releaseAll();
}

// --------------------------------------------------------------------- binding

void TimeSeriesPanel::releaseAll()
{
    releaseBoundSignals(traces_, *source_);
}

void TimeSeriesPanel::applyPresentation(Trace& trace) const
{
    trace.color = qt_helpers::toQColor(trace.binding.color, kDefaultTrace);
    switch (trace.binding.display)
    {
        case trace_display_t::automatic:
            trace.lane = trace.states.is_state;
            break;
        case trace_display_t::line:
            trace.lane = false;
            break;
        case trace_display_t::lane:
            trace.lane = true;
            break;
    }
}

std::unique_ptr<TimeSeriesPanel::Trace> TimeSeriesPanel::makeTrace(const signal_binding_t& binding)
{
    auto trace = makeBoundSignal<Trace>(*source_, binding, history_seconds_,
                                        kMaxPointsPerSignal, kStagingCapacity, cfg_.title);
    applyPresentation(*trace);
    return trace;
}

// Bring traces_ into line with cfg_.traces, keeping the buffer of every trace
// that is still there. The rule and the data-loss story it prevents live in
// bound_signal_list.h -- the table panel had the identical bug, fixed twice.
void TimeSeriesPanel::syncTraces()
{
    syncBoundSignals(traces_, cfg_.traces, *source_,
                     [this](const signal_binding_t& binding) { return makeTrace(binding); },
                     [this](Trace& trace) { applyPresentation(trace); });

    // Maintained here so plotRect() -- called per paint and per gesture -- can
    // read it instead of scanning the config every time.
    has_right_axis_ = std::any_of(cfg_.traces.begin(), cfg_.traces.end(),
                                  [](const signal_binding_t& binding)
                                  { return binding.right_axis; });

    update();
}

void TimeSeriesPanel::rebindAll()
{
    // The wholesale version, for the two cases where a buffer genuinely cannot
    // be carried over: a different SOURCE issued the handles, or the retention
    // the buffers were built with has changed.
    releaseAll();
    syncTraces();
}

void TimeSeriesPanel::applyConfig(const config_t& cfg)
{
    cfg_ = cfg;
    syncTraces();
    emit configChanged();
}

void TimeSeriesPanel::rebindTo(DataSource& source)
{
    if (&source == source_)
    {
        return;
    }

    // AGAINST THE OLD SOURCE, before the pointer moves. A handle means nothing
    // to a source that did not issue it, and repointing first would leave every
    // subscription on the old one alive -- decoding samples nothing will ever
    // draw. The window destroys the old source only after this returns, which
    // is what makes the release below legal.
    releaseAll();

    source_ = &source;
    rebindAll();
}

void TimeSeriesPanel::setHistorySeconds(double seconds)
{
    if (seconds == history_seconds_)
    {
        return;
    }
    history_seconds_ = seconds;

    // The buffers carry their retention at construction, so this has to rebuild
    // them -- and that discards whatever they had collected. Honest rather than
    // convenient: a buffer cannot grow a past it never recorded, and pretending
    // a shortened window kept its data would be worse than losing it.
    rebindAll();
}

bool TimeSeriesPanel::acceptsBinding(const BindingCandidate& candidate) const
{
    // A field, and one an expression can turn into a number. A topic-level
    // candidate belongs to some other kind of panel -- a video one, say -- and
    // this is where that gets decided, without the browser or the drag knowing
    // anything about panel types.
    if (candidate.isTopicLevel())
    {
        return false;
    }
    if (!candidate.isNumeric())
    {
        return false;
    }
    return !candidate.zenoh_key.empty() && !candidate.schema_name.empty();
}

bool TimeSeriesPanel::addBinding(const BindingCandidate& candidate)
{
    if (!acceptsBinding(candidate))
    {
        return false;
    }

    const auto schema = reflection::enum_traits<pub_sub::schema_type_t>::try_from_string(
        candidate.schema_name);
    if (!schema)
    {
        SPDLOG_WARN("Panel '{}': schema '{}' is not in the registry.", cfg_.title,
                    candidate.schema_name);
        return false;
    }

    signal_binding_t binding;
    binding.zenoh_key = candidate.zenoh_key;
    binding.schema_type = *schema;
    // The degenerate expression: just read the field. Editable afterwards.
    binding.value_expression = candidate.defaultExpression();
    binding.label = candidate.field_name;

    // Already plotted? Adding it twice draws the same line on top of itself and
    // doubles the decode cost for nothing.
    const auto duplicate = std::find_if(
        cfg_.traces.begin(), cfg_.traces.end(), [&binding](const signal_binding_t& existing) {
            return existing.zenoh_key == binding.zenoh_key &&
                   existing.schema_type == binding.schema_type &&
                   existing.value_expression == binding.value_expression;
        });
    if (duplicate != cfg_.traces.end())
    {
        return false;
    }

    // Cycle through a readable palette rather than making every trace the
    // config default, which would make a second signal invisible against the
    // first. Chosen for contrast against the dark plot background and for
    // staying distinguishable to the most common colour-vision deficiencies.
    static const char* kPalette[] = {"#4FC3F7", "#FFB74D", "#81C784", "#E57373",
                                     "#BA68C8", "#FFF176", "#4DD0E1", "#F06292"};
    binding.color = helpers::Color(
        std::string(kPalette[cfg_.traces.size() % (sizeof(kPalette) / sizeof(kPalette[0]))]));

    cfg_.traces.push_back(binding);
    syncTraces();
    emit configChanged();
    return true;
}

std::vector<QString> TimeSeriesPanel::bindingLabels() const
{
    std::vector<QString> labels;
    labels.reserve(traces_.size());
    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        labels.push_back(trace->displayLabel());
    }
    return labels;
}

std::size_t TimeSeriesPanel::unboundBindingCount() const
{
    return static_cast<std::size_t>(std::count_if(traces_.begin(), traces_.end(),
                                                  [](const std::unique_ptr<Trace>& trace)
                                                  { return !trace->bound; }));
}

bool TimeSeriesPanel::removeBinding(std::size_t index)
{
    if (index >= cfg_.traces.size())
    {
        return false;
    }
    cfg_.traces.erase(cfg_.traces.begin() + static_cast<std::ptrdiff_t>(index));
    syncTraces();
    emit configChanged();
    return true;
}

void TimeSeriesPanel::setTimeBase(TimeBase* time_base)
{
    if (time_base_ != nullptr)
    {
        disconnect(time_base_, nullptr, this, nullptr);
    }

    time_base_ = time_base;
    if (time_base_ == nullptr)
    {
        return;
    }

    connect(time_base_, &TimeBase::frame, this, &TimeSeriesPanel::onFrame);
    connect(time_base_, &TimeBase::changed, this, [this]() {
        update();
    });
    connect(time_base_, &TimeBase::cursorMoved, this, [this]() { update(); });
}

QString TimeSeriesPanel::title() const
{
    return QString::fromStdString(cfg_.title);
}

void TimeSeriesPanel::onFrame()
{
    if (time_base_ == nullptr)
    {
        return;
    }

    // Draining is what moves samples from the producer's ring into the history
    // paint reads. It happens even while paused: the view freezes, the data
    // does not, so unpausing shows what arrived meanwhile rather than a gap.
    const double now = time_base_->source().now();
    std::size_t moved = 0;
    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        moved += trace->buffer->drain(now);
    }

    // Repaint only when the picture can differ: samples arrived, or the shared
    // window/cursor moved (a following live edge moves every tick; a paused
    // review does not). A parked view of a recording used to re-decimate and
    // re-autoscale every trace 30 times a second to redraw identical pixels.
    // Every other cause of change -- a config apply, a gesture, a resize --
    // already calls update() at its own site.
    const double view_end = time_base_->viewEnd();
    const double view_begin = time_base_->viewBegin();
    const std::optional<double> cursor = time_base_->cursor();
    const bool view_moved = view_end != last_frame_end_ || view_begin != last_frame_begin_ ||
                            cursor != last_frame_cursor_;
    last_frame_end_ = view_end;
    last_frame_begin_ = view_begin;
    last_frame_cursor_ = cursor;

    if (moved > 0 || view_moved)
    {
        update();
    }
}

TimeSeriesPanel::stats_t TimeSeriesPanel::stats() const
{
    stats_t all;
    all.traces.reserve(traces_.size());

    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        trace_stats_t stats;
        stats.label = trace->displayLabel().toStdString();
        stats.bound = trace->bound;
        stats.lane = trace->lane;
        stats.received = trace->buffer->received();
        stats.dropped = trace->buffer->dropped();

        const SampleHistory& history = trace->buffer->history();
        stats.retained = history.size();
        if (!history.empty())
        {
            stats.has_data = true;
            stats.t_first = history.oldest().t;
            stats.t_last = history.newest().t;
            stats.last = history.newest().v;
            stats.min = std::numeric_limits<double>::infinity();
            stats.max = -std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < history.size(); ++i)
            {
                stats.min = std::min(stats.min, history[i].v);
                stats.max = std::max(stats.max, history[i].v);
            }
        }

        all.traces.push_back(std::move(stats));
    }

    return all;
}

// -------------------------------------------------------------------- geometry

QRectF TimeSeriesPanel::plotRect() const
{
    const double w = static_cast<double>(width());
    const double h = static_cast<double>(height());

    // The right gutter only exists when something is scaled against the right
    // axis, so a single-axis plot gets the full width rather than a permanent
    // empty strip. has_right_axis_ is maintained by syncTraces() -- this is
    // called several times per paint and from every gesture, so the any_of it
    // used to run here was recomputed constantly for an answer that changes
    // only with the config.
    const double right = has_right_axis_ ? kRightAxisGutter : kRightGutter;

    QRectF area(kLeftGutter, kTopGutter, w - kLeftGutter - right,
                h - kTopGutter - kBottomGutter - lanesHeight());
    if (area.width() < 1.0 || area.height() < 1.0)
    {
        // A layout pass can hand us a widget too small for the gutters. Return
        // something degenerate but valid so the paint path does not divide by
        // zero; it will draw nothing and be resized again in a moment.
        return QRectF(kLeftGutter, kTopGutter, 1.0, 1.0);
    }
    return area;
}

int TimeSeriesPanel::laneCount() const
{
    return static_cast<int>(std::count_if(traces_.begin(), traces_.end(),
                                          [](const std::unique_ptr<Trace>& trace)
                                          { return trace->lane; }));
}

double TimeSeriesPanel::lanesHeight() const
{
    const int lanes = laneCount();
    return lanes == 0 ? 0.0 : (kLaneGap + lanes * kLaneHeight);
}

QRectF TimeSeriesPanel::lanesRect() const
{
    const int lanes = laneCount();
    if (lanes == 0)
    {
        return QRectF();
    }
    const QRectF area = plotRect();
    return QRectF(area.left(), area.bottom() + kLaneGap, area.width(), lanes * kLaneHeight);
}

void TimeSeriesPanel::paintLanes(QPainter& painter, const QRectF& lanes)
{
    if (!lanes.isValid() || lanes.height() < 1.0)
    {
        return;
    }

    const TimeAxis axis = timeAxis();
    const QFontMetricsF metrics(painter.font());

    double top = lanes.top();
    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        if (!trace->lane)
        {
            continue;
        }

        const QRectF row(lanes.left(), top, lanes.width(), kLaneHeight);
        top += kLaneHeight;

        painter.fillRect(row, kLaneRowFill);

        const SampleHistory& history = trace->buffer->history();
        if (history.empty())
        {
            continue;
        }

        // Start one sample BEFORE the window. A state that changed before the
        // left edge is still in force at the left edge, and beginning at the
        // first sample inside the window would leave a gap that reads as "no
        // data" rather than as "unchanged for a while".
        std::size_t index = history.lowerBound(drawn_begin_);
        if (index > 0)
        {
            --index;
        }

        // Runs of equal value, drawn as one band each. A per-sample rectangle
        // would be one draw call per sample per frame, and at 30 Hz over a
        // window holding thousands of them that is the whole frame budget --
        // for a channel that by its nature changes a handful of times.
        while (index < history.size() && history[index].t <= drawn_end_)
        {
            const double value = history[index].v;
            const double run_begin = history[index].t;

            // Bounded by drawn_end_ like the outer loop: the run straddling the
            // right edge is clamped to drawn_end_ below anyway, and without the
            // bound this walked to the end of RETAINED history -- with the view
            // parked in the past, that is every sample between the window and
            // the live edge, scanned per lane per frame, for pixels that never
            // draw.
            std::size_t next = index + 1;
            while (next < history.size() && history[next].t <= drawn_end_ &&
                   history[next].v == value)
            {
                ++next;
            }

            // Zero-order hold: the state runs until the sample that changed it,
            // or to the right edge if nothing did.
            const double run_end =
                (next < history.size()) ? history[next].t : std::max(drawn_end_, run_begin);

            const double x0 = axis.toClampedX(std::max(run_begin, drawn_begin_));
            const double x1 = axis.toClampedX(std::min(run_end, drawn_end_));
            index = next;

            if (x1 <= x0)
            {
                continue;
            }

            const QRectF band(x0, row.top() + 1.0, x1 - x0, row.height() - 2.0);
            const auto ordinal = static_cast<long long>(std::llround(value));
            painter.fillRect(band, stateColor(ordinal < 0 ? 0 : static_cast<std::size_t>(ordinal)));

            // The name, when the band is wide enough to hold it. A band too
            // narrow to label is still a visible colour change, which is the
            // thing you are looking for when you scan a lane.
            const QString label = trace->states.label(value);
            if (band.width() > metrics.horizontalAdvance(label) + 8.0)
            {
                painter.setPen(kLaneTextDark);
                painter.drawText(band, Qt::AlignCenter, label);
            }
        }

        // The channel's own name, over the band rather than in the left gutter:
        // the gutter belongs to the value axis, and a lane has no value axis.
        painter.setPen(kForeground);
        painter.drawText(row.adjusted(6.0, 0.0, 0.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
                         trace->displayLabel());

        painter.setPen(QPen(kFrame, 1.0));
        painter.drawRect(row);
    }
}

TimeAxis TimeSeriesPanel::timeAxis() const
{
    const QRectF area = plotRect();
    TimeAxis axis;
    axis.t0 = drawn_begin_;
    axis.t1 = drawn_end_;
    axis.x0 = area.left();
    axis.x1 = area.right();
    return axis;
}

void TimeSeriesPanel::computeYRange(double& y_min, double& y_max, bool right_axis) const
{
    if (!cfg_.autoscale_y)
    {
        y_min = cfg_.y_min;
        y_max = cfg_.y_max;
        return;
    }

    y_min = std::numeric_limits<double>::infinity();
    y_max = -std::numeric_limits<double>::infinity();

    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        if (trace->binding.right_axis != right_axis)
        {
            continue;
        }

        // A lane is not on the value axis and must not stretch it. An enum
        // whose ordinals run 0..7 sitting in the same autoscale as rpm would
        // flatten the rpm trace against the top of the plot -- the state
        // channel would be invisible AND it would ruin the trace beside it.
        if (trace->lane)
        {
            continue;
        }

        // From the decimation columns, not the samples: decimateTraces() has
        // already reduced the window, and each column's min/max covers every
        // sample in it, so this is the same answer for the width of the widget
        // rather than the size of the window.
        for (const ColumnStats& stats : trace->columns)
        {
            if (!stats.has_data)
            {
                continue;
            }
            y_min = std::min(y_min, stats.min);
            y_max = std::max(y_max, stats.max);
        }
    }

    if (!std::isfinite(y_min) || !std::isfinite(y_max))
    {
        // Nothing visible yet. A unit range beats a degenerate one: the axis
        // labels are meaningless either way, but this one does not divide by
        // zero.
        y_min = 0.0;
        y_max = 1.0;
        return;
    }

    if (y_max - y_min < 1e-9)
    {
        // A flat signal. Centre it rather than drawing it on the frame, which
        // is what a zero-height range does and what makes a constant look like
        // a missing trace.
        const double centre = y_min;
        const double pad = std::max(std::abs(centre) * 0.1, 0.5);
        y_min = centre - pad;
        y_max = centre + pad;
        return;
    }

    // A little headroom, so the extremes are not drawn on the frame itself.
    const double pad = (y_max - y_min) * 0.05;
    y_min -= pad;
    y_max += pad;
}

// -------------------------------------------------------------------- painting

void TimeSeriesPanel::decimateTraces(const QRectF& area)
{
    const std::size_t columns = static_cast<std::size_t>(std::max(1.0, area.width()));
    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        if (!trace->bound || trace->lane)
        {
            trace->filled = 0;
            trace->columns.clear();
            continue;
        }
        trace->filled = decimateMinMax(trace->buffer->history(), drawn_begin_, drawn_end_,
                                       columns, trace->columns);
    }
}

void TimeSeriesPanel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF area = plotRect();

    // The window to draw. A panel that does not follow the shared time base
    // still shares its clock -- only the width differs -- so two panels never
    // disagree about what "now" is.
    if (time_base_ != nullptr)
    {
        drawn_end_ = time_base_->viewEnd();
        drawn_begin_ = drawn_end_ - (cfg_.follow_time_base ? time_base_->windowSeconds()
                                                           : cfg_.window_seconds);
    }
    else
    {
        drawn_end_ = 0.0;
        drawn_begin_ = -cfg_.window_seconds;
    }

    decimateTraces(area);

    computeYRange(drawn_y_min_, drawn_y_max_, /*right_axis=*/false);
    if (has_right_axis_)
    {
        computeYRange(drawn_y2_min_, drawn_y2_max_, /*right_axis=*/true);
    }

    painter.fillRect(rect(), kBackground);

    if (cfg_.show_grid)
    {
        paintGrid(painter, area, drawn_y_min_, drawn_y_max_);
    }

    painter.setPen(QPen(kFrame, 1.0));
    painter.drawRect(area);

    paintTraces(painter, area);
    paintLanes(painter, lanesRect());
    paintCursor(painter, area);

    // The rubber band, drawn rather than made a QRubberBand: that is a
    // near-top-level widget, which the offscreen platform handles badly, and
    // everything else here is already painted.
    if (drag_ == Drag::Band)
    {
        const TimeAxis axis = timeAxis();
        const double x0 = axis.toClampedX(std::min(band_begin_, band_end_));
        const double x1 = axis.toClampedX(std::max(band_begin_, band_end_));
        const QRectF band(x0, area.top(), x1 - x0, area.height());

        painter.fillRect(band, QColor(224, 228, 234, 40));
        painter.setPen(QPen(kForeground, 1.0));
        painter.drawLine(QPointF(x0, area.top()), QPointF(x0, area.bottom()));
        painter.drawLine(QPointF(x1, area.top()), QPointF(x1, area.bottom()));
    }

    if (cfg_.show_legend)
    {
        paintLegend(painter);
    }
}

void TimeSeriesPanel::paintGrid(QPainter& painter, const QRectF& area, double y_min, double y_max)
{
    const QPen grid_pen(kGridLine, 1.0);
    const QPen label_pen(kGridLabel, 1.0);
    const QFontMetricsF metrics(painter.font());

    // Vertical axis.
    const double y_step = niceStep((y_max - y_min) / kTargetYTicks);
    const double y_first = std::ceil(y_min / y_step) * y_step;
    for (double value = y_first; value <= y_max; value += y_step)
    {
        const double y = area.bottom() - (value - y_min) / (y_max - y_min) * area.height();
        painter.setPen(grid_pen);
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));

        painter.setPen(label_pen);
        const QString text = formatValue(value);
        painter.drawText(QPointF(area.left() - metrics.horizontalAdvance(text) - 6.0,
                                 y + metrics.height() / 3.0),
                         text);
    }

    // Right-hand axis: labels only, no grid lines. A second set of horizontal
    // lines at different heights turns the plot into a lattice and makes both
    // scales harder to read than either alone.
    if (has_right_axis_)
    {
        const double y2_step = niceStep((drawn_y2_max_ - drawn_y2_min_) / kTargetYTicks);
        const double y2_first = std::ceil(drawn_y2_min_ / y2_step) * y2_step;
        painter.setPen(label_pen);
        for (double value = y2_first; value <= drawn_y2_max_; value += y2_step)
        {
            const double y = area.bottom() -
                             (value - drawn_y2_min_) / (drawn_y2_max_ - drawn_y2_min_) *
                                 area.height();
            painter.drawText(QPointF(area.right() + 6.0, y + metrics.height() / 3.0),
                             formatValue(value));
        }
    }

    // Time axis, labelled relative to the right edge -- "-10 s" reads as "ten
    // seconds ago", which is what someone watching live data wants. Absolute
    // times on a steady_clock epoch would mean nothing at all.
    const double span = drawn_end_ - drawn_begin_;
    if (span <= 0.0)
    {
        return;
    }
    const double t_step = niceStep(span / kTargetXTicks);
    for (double back = 0.0; back <= span; back += t_step)
    {
        const double t = drawn_end_ - back;
        const double x = area.left() + (t - drawn_begin_) / span * area.width();

        painter.setPen(grid_pen);
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));

        painter.setPen(label_pen);
        const QString text = back == 0.0 ? QStringLiteral("0")
                                         : QStringLiteral("-%1").arg(formatValue(back));
        // BELOW THE LANES, not below the plot. The plot's bottom edge used to be
        // the bottom of the widget's content, so a label just under it landed in
        // the gutter; with lanes present that same spot is the first lane, and
        // the label was drawn there and then painted over -- half a tick label
        // peeking out from behind a state band.
        painter.drawText(QPointF(x - metrics.horizontalAdvance(text) / 2.0,
                                 area.bottom() + lanesHeight() + metrics.height()),
                         text);
    }
}

void TimeSeriesPanel::paintTraces(QPainter& painter, const QRectF& area)
{
    const double span = drawn_end_ - drawn_begin_;
    if (span <= 0.0)
    {
        return;
    }

    painter.save();
    painter.setClipRect(area);

    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        if (!trace->bound || trace->lane)
        {
            continue;
        }

        // Each trace is scaled against the axis it was assigned to. This is the
        // point of having two: engine rpm and oil pressure differ by three
        // orders of magnitude, and on one scale the smaller of them is a flat
        // line along the bottom whose shape you cannot see at all.
        const double axis_min = trace->binding.right_axis ? drawn_y2_min_ : drawn_y_min_;
        const double axis_max = trace->binding.right_axis ? drawn_y2_max_ : drawn_y_max_;
        const double y_span = axis_max - axis_min;
        if (y_span <= 0.0)
        {
            continue;
        }

        if (trace->filled == 0)
        {
            continue;
        }

        const auto toY = [&](double value) {
            return area.bottom() - (value - axis_min) / y_span * area.height();
        };

        painter.setPen(QPen(trace->color, 1.5));

        // Two things per column: a vertical segment spanning the values seen in
        // it, and a join to the previous column. The segment is what preserves
        // spikes through decimation; the join is what stops a smooth signal
        // looking like a comb.
        bool have_previous = false;
        double previous_x = 0.0;
        double previous_last = 0.0;

        for (std::size_t i = 0; i < trace->columns.size(); ++i)
        {
            const ColumnStats& stats = trace->columns[i];
            if (!stats.has_data)
            {
                continue;
            }

            const double x = area.left() + static_cast<double>(i);

            if (have_previous)
            {
                painter.drawLine(QPointF(previous_x, toY(previous_last)),
                                 QPointF(x, toY(stats.first)));
            }

            if (stats.max > stats.min)
            {
                painter.drawLine(QPointF(x, toY(stats.min)), QPointF(x, toY(stats.max)));
            }

            have_previous = true;
            previous_x = x;
            previous_last = stats.last;
        }
    }

    painter.restore();
}

void TimeSeriesPanel::paintCursor(QPainter& painter, const QRectF& area)
{
    if (time_base_ == nullptr || !time_base_->cursor())
    {
        return;
    }

    const double t = *time_base_->cursor();
    const double span = drawn_end_ - drawn_begin_;
    if (span <= 0.0 || t < drawn_begin_ || t > drawn_end_)
    {
        // The cursor is over another panel showing a different window. Drawing
        // it clamped to our edge would claim a reading we do not have.
        return;
    }

    const double x = area.left() + (t - drawn_begin_) / span * area.width();
    painter.setPen(QPen(kForeground, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
}

void TimeSeriesPanel::paintLegend(QPainter& painter)
{
    if (traces_.empty())
    {
        painter.setPen(kLegendMuted);
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No signals.\nDrag one here from the browser."));
        return;
    }

    const QFontMetricsF metrics(painter.font());
    const double line_height = metrics.height() + 2.0;
    double y = kTopGutter + line_height;

    // Under a cursor the legend reads out the value at that instant instead of
    // the latest one. That is the whole reason the cursor is shared: three
    // panels all answering "what was this at t = 12.48?" at once.
    const std::optional<double> cursor =
        time_base_ != nullptr ? time_base_->cursor() : std::nullopt;

    for (const std::unique_ptr<Trace>& trace : traces_)
    {
        const double x = kLeftGutter + 8.0;

        painter.setPen(Qt::NoPen);
        painter.setBrush(trace->color);
        painter.drawRect(QRectF(x, y - metrics.ascent() * 0.7, 8.0, 8.0));
        painter.setBrush(Qt::NoBrush);

        QString text = trace->displayLabel();
        if (trace->binding.right_axis)
        {
            // Which scale a trace is read against is not guessable from the
            // picture once there are two, so the legend has to say.
            text += tr(" [R]");
        }
        if (!trace->bound)
        {
            text += tr("  (unbound)");
        }
        else if (trace->buffer->dropped() > 0)
        {
            // The one counter that means the trace is LYING about the data --
            // samples arrived and were thrown away, so gaps and flat spots may
            // be the drop, not the signal. It was agent-only (sample_stats)
            // before; a GUI user could not tell a decimated-but-honest trace
            // from a lossy one.
            text += tr("  (! %1 dropped)").arg(trace->buffer->dropped());
        }
        if (trace->bound)
        {
            const SampleHistory& history = trace->buffer->history();
            if (history.empty())
            {
                text += tr("  --");
            }
            else
            {
                double value = history.newest().v;
                if (cursor)
                {
                    // Nearest sample at or before the cursor: a plot should not
                    // invent a reading between two samples.
                    const std::size_t at = history.lowerBound(*cursor);
                    if (at < history.size() && history[at].t == *cursor)
                    {
                        value = history[at].v;
                    }
                    else if (at > 0)
                    {
                        value = history[at - 1].v;
                    }
                }
                text += QStringLiteral("  ") + formatValue(value);
                if (!trace->binding.units.empty())
                {
                    text += QStringLiteral(" ") + QString::fromStdString(trace->binding.units);
                }
            }
        }

        painter.setPen(trace->bound ? kLegendBound : kLegendUnbound);
        painter.drawText(QPointF(x + 14.0, y), text);
        y += line_height;
    }
}

// -------------------------------------------------------- hover and gestures

void TimeSeriesPanel::mouseMoveEvent(QMouseEvent* event)
{
    if (time_base_ == nullptr)
    {
        Panel::mouseMoveEvent(event);
        return;
    }

    const TimeAxis axis = timeAxis();

    // A drag in progress owns the pointer: it does NOT also move the shared
    // cursor. Letting it would drag every other panel's readout along with the
    // window and make the legend flicker through values nobody asked to see.
    if (drag_ != Drag::None)
    {
        if (drag_ == Drag::Pending && dragExceededThreshold(event->pos()))
        {
            drag_ = (event->modifiers() & Qt::ShiftModifier) ? Drag::Band : Drag::Pan;
            if (drag_ == Drag::Pan)
            {
                setCursor(Qt::ClosedHandCursor);
            }
            else
            {
                band_begin_ = axis.toT(drag_origin_.x());
                band_end_ = band_begin_;
            }
        }

        if (drag_ == Drag::Pan)
        {
            // From the DELTA since the last move, not from the origin: the
            // window slides underneath as we go, so measuring against the origin
            // would re-apply the whole offset every frame.
            const double dx = static_cast<double>(event->pos().x() - drag_last_.x());
            time_base_->panBy(-dx * axis.secondsPerPixel());
            drag_last_ = event->pos();
        }
        else if (drag_ == Drag::Band)
        {
            band_end_ = axis.toT(event->position().x());
            update();
        }

        Panel::mouseMoveEvent(event);
        return;
    }

    const double x = event->position().x();
    if (x < axis.x0 || x > axis.x1)
    {
        time_base_->setCursor(std::nullopt);
        Panel::mouseMoveEvent(event);
        return;
    }

    time_base_->setCursor(axis.toT(x));
    Panel::mouseMoveEvent(event);
}

void TimeSeriesPanel::leaveEvent(QEvent* event)
{
    if (time_base_ != nullptr)
    {
        time_base_->setCursor(std::nullopt);
    }
    Panel::leaveEvent(event);
}

bool TimeSeriesPanel::dragExceededThreshold(const QPoint& pos) const
{
    return (pos - drag_origin_).manhattanLength() >= QApplication::startDragDistance();
}

void TimeSeriesPanel::mousePressEvent(QMouseEvent* event)
{
    if (time_base_ == nullptr || event->button() != Qt::LeftButton || !timeAxis().usable())
    {
        Panel::mousePressEvent(event);
        return;
    }

    // Pending, not Pan: a press that never travels is a click, and a click must
    // not move a window every other panel is looking at.
    drag_ = Drag::Pending;
    drag_origin_ = event->pos();
    drag_last_ = event->pos();
    Panel::mousePressEvent(event);
}

void TimeSeriesPanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (drag_ == Drag::None)
    {
        Panel::mouseReleaseEvent(event);
        return;
    }

    const Drag was = drag_;
    drag_ = Drag::None;
    unsetCursor();

    if (time_base_ != nullptr)
    {
        if (was == Drag::Band)
        {
            const double lo = std::min(band_begin_, band_end_);
            const double hi = std::max(band_begin_, band_end_);

            // A band narrower than a couple of pixels is a shift-click, not a
            // range. Zooming to it would drop the view to the minimum span for
            // what the user experienced as a mis-click.
            if (hi - lo > timeAxis().secondsPerPixel() * 2.0)
            {
                time_base_->setView(lo, hi);
            }
            update();
        }
    }

    Panel::mouseReleaseEvent(event);
}

void TimeSeriesPanel::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (time_base_ == nullptr)
    {
        Panel::mouseDoubleClickEvent(event);
        return;
    }

    // "Show me everything" on a recording; "catch up with the bus" on a live
    // source. fitAll() means both -- a live source's available range ends at
    // now(), so fitting it lands against the live edge and re-arms following.
    time_base_->fitAll();

    // The vertical half of the same gesture. Shift+wheel turns autoscale off,
    // and this is the one-gesture way back -- without it a stray scroll leaves
    // a panel pinned to a range the user has to find in a config dialog.
    if (!cfg_.autoscale_y)
    {
        cfg_.autoscale_y = true;
        emit configChanged();
    }

    update();
    Panel::mouseDoubleClickEvent(event);
}

void TimeSeriesPanel::wheelEvent(QWheelEvent* event)
{
    const TimeAxis axis = timeAxis();
    if (time_base_ == nullptr || !axis.usable())
    {
        Panel::wheelEvent(event);
        return;
    }

    const double delta = static_cast<double>(event->angleDelta().y());
    if (delta == 0.0)
    {
        Panel::wheelEvent(event);
        return;
    }

    // A tuned exponential rather than a fixed step, so a slow scroll is precise
    // and a fast one still covers ground. The sign is inverted because a wheel
    // pushed away is positive and means "closer", i.e. a SMALLER span.
    const double factor = std::pow(1.0015, -delta);

    if (event->modifiers() & Qt::ShiftModifier)
    {
        zoomValueAxis(event->position().y(), factor);
    }
    else
    {
        // About the pointer, so the sample under it stays under it. That one
        // property is what makes a wheel feel like a map rather than a slider.
        time_base_->zoomAt(axis.toT(event->position().x()), factor);
    }

    event->accept();
}

void TimeSeriesPanel::zoomValueAxis(double at_y, double factor)
{
    const QRectF area = plotRect();
    if (area.height() < 1.0)
    {
        return;
    }

    // Read the range off what was DRAWN, not off the config: with autoscale on
    // the config's y_min/y_max are stale defaults, and zooming from them would
    // jump the axis somewhere unrelated on the first scroll.
    const double lo = drawn_y_min_;
    const double hi = drawn_y_max_;
    const double span = hi - lo;
    if (span <= 0.0 || !std::isfinite(span))
    {
        return;
    }

    // Same pivot rule as the time axis, but the pixel axis runs downwards.
    const double at = std::clamp((area.bottom() - at_y) / area.height(), 0.0, 1.0);
    const double value = lo + at * span;
    const double new_span = span * factor;

    cfg_.autoscale_y = false;
    cfg_.y_min = value - at * new_span;
    cfg_.y_max = value + (1.0 - at) * new_span;

    // A navigation gesture that genuinely changes the saved configuration, so
    // the workspace really is dirty. Double-click is the way back.
    emit configChanged();
    update();
}

}  // namespace scope

#include "time_series/moc_time_series_panel.cpp"
