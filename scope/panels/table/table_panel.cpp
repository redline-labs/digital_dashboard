#include "table/table_panel.h"

#include "scope/data_source.h"
#include "scope/time_base.h"
#include "scope/value_format.h"

// For the reflection-generated operator==, which is what lets applyConfig() tell
// a changed BINDING from a changed column width without a hand-written
// comparison that would rot against the config's fields.
#include "config_codec/config_yaml.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace scope
{

namespace
{

// A readout holds one sample per row, so the point cap only has to be large
// enough that a cursor anywhere in the view lands inside it. Generous anyway:
// the memory is a row's worth, not a plot's.
constexpr std::size_t kMaxPointsPerSignal = 120000;

// The same headroom the plot uses, for the same reason -- one GUI tick at 30 Hz
// is 33 ms and 4096 slots is over a second at 1 kHz. Overflow means the GUI
// thread is wedged, and then nothing is being drawn either way.
constexpr std::size_t kStagingCapacity = 4096;

constexpr double kPad = 8.0;
constexpr double kRowPad = 6.0;
constexpr double kColumnGap = 10.0;

// Column widths, in logical pixels. A dock dragged narrow squeezes the NAME
// column: a truncated name is still identifiable from its position and a
// truncated value is a wrong number.
constexpr double kMinNameWidth = 40.0;
constexpr double kMinValueWidth = 96.0;
constexpr double kUnitsWidth = 52.0;
constexpr double kAgeWidth = 64.0;

// The value column GROWS with the panel, up to this -- until the user drags it,
// after which cfg_.value_width is what it is. A fixed 96 px fits any number and
// elides half the state names on this bus -- "airplayHandshake" and "recording"
// are the readings that matter most, and they were the ones cut off while 500 px
// of the panel sat empty. An enum is the reason this panel exists; the column it
// lands in cannot be sized for a float.
constexpr double kMaxValueWidth = 240.0;
constexpr double kValueShare = 0.40;

// How close the pointer has to be to a divider to grab it. Four pixels each
// side: a one-pixel target is unhittable, and much more than this makes the
// dividers feel like they are pulling the cursor around.
constexpr double kGrabTolerance = 4.0;

const QColor kBackground("#14161A");
const QColor kHeaderText("#8A94A6");
const QColor kRule("#232830");
const QColor kName("#C8CEDA");
const QColor kValue("#E0E4EA");
const QColor kUnbound("#8A6060");
const QColor kStale("#D08A50");
const QColor kEmptyHint("#5A6270");

// Do these two rows name THE SAME SIGNAL? The binding triple and nothing else:
// a label, a format, a units suffix or a decimal count are presentation, and
// changing one must not cost the row its history.
//
// The same triple SignalKey is built from, deliberately -- this is the identity
// the source issues a handle against, so two rows that compare equal here are
// two rows the source cannot tell apart.
bool sameSignal(const table_row_t& lhs, const table_row_t& rhs)
{
    return lhs.zenoh_key == rhs.zenoh_key && lhs.schema_type == rhs.schema_type &&
           lhs.value_expression == rhs.value_expression;
}

// How long ago, at a width a glance can compare. Milliseconds below a second
// because that is the range a healthy bus lives in, and minutes above one
// because "412.7 s" is a number the reader has to divide.
QString formatAge(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
    {
        return QStringLiteral("--");
    }
    if (seconds < 1.0)
    {
        return QString::number(seconds * 1000.0, 'f', 0) + QStringLiteral(" ms");
    }
    if (seconds < 60.0)
    {
        return QString::number(seconds, 'f', 1) + QStringLiteral(" s");
    }
    return QString::number(seconds / 60.0, 'f', 1) + QStringLiteral(" m");
}

}  // namespace

// One row: its configuration, where its samples land, the handle that keeps it
// bound, and what its states are called.
struct TablePanel::Row
{
    table_row_t binding;
    std::shared_ptr<SignalBuffer> buffer;
    SignalHandle handle = kInvalidSignal;

    // Resolved at bind time from the schema, not at paint time: it reads the
    // registry and builds a JSON description, which is fine once per binding and
    // absurd at 30 Hz.
    StateNames states;

    // Print the state's name rather than the number. From the row's `format`
    // and, when that is `automatic`, from what the field turned out to be.
    bool as_state = false;

    // False when binding failed -- a bad expression, a non-numeric field, a
    // subscription that would not declare. The row says so rather than showing
    // an empty cell that looks like a quiet signal.
    bool bound = false;

    QString displayLabel() const
    {
        if (!binding.label.empty())
        {
            return QString::fromStdString(binding.label);
        }
        return QString::fromStdString(binding.value_expression);
    }
};

TablePanel::TablePanel(const config_t& cfg, DataSource& source, double history_seconds,
                       QWidget* parent) :
    Panel(parent), cfg_(cfg), source_(&source), history_seconds_(history_seconds)
{
    // For the divider hover cursor, which needs no button held.
    setMouseTracking(true);
    setMinimumSize(160, 60);
    setAutoFillBackground(false);
    rebindAll();
}

TablePanel::~TablePanel()
{
    releaseAll();
}

// --------------------------------------------------------------------- binding

void TablePanel::releaseAll()
{
    for (const std::unique_ptr<Row>& row : rows_)
    {
        if (row->handle != kInvalidSignal)
        {
            source_->release(row->handle);
        }
    }
    rows_.clear();
}

void TablePanel::applyFormat(Row& row) const
{
    switch (row.binding.format)
    {
        case cell_format_t::automatic:
            row.as_state = row.states.is_state;
            break;
        case cell_format_t::number:
        case cell_format_t::hex:
            row.as_state = false;
            break;
        case cell_format_t::state:
            row.as_state = true;
            break;
    }
}

std::unique_ptr<TablePanel::Row> TablePanel::makeRow(const table_row_t& binding)
{
    auto row = std::make_unique<Row>();
    row->binding = binding;
    row->buffer =
        std::make_shared<SignalBuffer>(history_seconds_, kMaxPointsPerSignal, kStagingCapacity);

    SignalKey key;
    key.zenoh_key = binding.zenoh_key;
    key.schema_type = binding.schema_type;
    key.value_expression = binding.value_expression;

    row->handle = source_->bind(key, row->buffer);
    row->bound = row->handle != kInvalidSignal;

    // Resolved here rather than at paint time: it reads the schema registry and
    // builds a JSON description, which is fine once per binding and absurd at
    // 30 Hz.
    row->states = resolveStateNames(binding.schema_type, binding.value_expression);
    applyFormat(*row);

    if (!row->bound)
    {
        // Already logged in detail by the evaluator; this says which panel.
        SPDLOG_WARN("Panel '{}': signal '{}' on '{}' could not be bound.", cfg_.title,
                    binding.value_expression, binding.zenoh_key);
    }

    return row;
}

// Bring rows_ into line with cfg_.rows, KEEPING THE BUFFER OF EVERY ROW THAT IS
// STILL THERE.
//
// This used to rebuild all of them, and that was a data-loss bug rather than an
// inefficiency. Adding a signal gave every OTHER row a brand-new empty buffer,
// so their history was gone -- and while the view is paused the readout instant
// is frozen in the past, where the new buffers have nothing. Every row in the
// table read "--", the panel looked dead, and resuming only fixed it from the
// resume point forwards because the history covering everything before it had
// been thrown away. Nothing logged it, because from the panel's point of view it
// had just bound successfully.
//
// Identity is the binding triple (see sameSignal). A row whose label, units,
// format or decimals changed is the same subscription with different
// presentation, so it keeps everything and only re-reads those fields.
void TablePanel::syncRows()
{
    std::vector<std::unique_ptr<Row>> previous;
    previous.swap(rows_);
    rows_.reserve(cfg_.rows.size());

    for (const table_row_t& binding : cfg_.rows)
    {
        // Moved out of `previous` when claimed, so the entry goes null and a
        // second row naming the same signal -- which only a hand-edited
        // workspace can produce -- gets its own binding rather than stealing
        // this one's buffer and leaving two rows sharing it.
        const auto match = std::find_if(previous.begin(), previous.end(),
                                        [&binding](const std::unique_ptr<Row>& row) {
                                            return row && sameSignal(row->binding, binding);
                                        });

        if (match == previous.end())
        {
            rows_.push_back(makeRow(binding));
            continue;
        }

        std::unique_ptr<Row> row = std::move(*match);
        row->binding = binding;  // Presentation may have changed; the signal did not.
        applyFormat(*row);
        rows_.push_back(std::move(row));
    }

    // Whatever was not claimed is genuinely gone, and its subscription with it.
    for (const std::unique_ptr<Row>& leftover : previous)
    {
        if (leftover && leftover->handle != kInvalidSignal)
        {
            source_->release(leftover->handle);
        }
    }

    clampScroll();
    update();
}

void TablePanel::rebindAll()
{
    // The wholesale version, for the two cases where a buffer genuinely cannot
    // be carried over: a different SOURCE issued the handles, or the retention
    // the buffers were built with has changed.
    releaseAll();
    syncRows();
}

void TablePanel::applyConfig(const config_t& cfg)
{
    // ONLY WHEN THE ROWS CHANGED, which is the difference from the plot's
    // applyConfig() and the reason this one can afford the check: a rebind
    // rebuilds every buffer and throws away the history in it, and this panel's
    // config is now mostly PRESENTATION -- a column width, a staleness limit, a
    // units toggle. Rebinding on those would mean a drag through
    // `scope.panel_set_config` silently emptied every row, so a cursor parked in
    // the past would read "--" until the buffers refilled. The comparison is the
    // reflection-generated operator==, so it cannot fall out of date with the
    // fields the way a hand-written one would.
    const bool rows_changed = !(cfg.rows == cfg_.rows);

    cfg_ = cfg;
    if (rows_changed)
    {
        syncRows();
    }
    else
    {
        clampScroll();
        update();
    }
    emit configChanged();
}

void TablePanel::rebindTo(DataSource& source)
{
    if (&source == source_)
    {
        return;
    }

    // AGAINST THE OLD SOURCE, before the pointer moves. A handle means nothing
    // to a source that did not issue it, and repointing first would leave every
    // subscription on the old one alive. The window destroys the old source only
    // after this returns, which is what makes the release below legal.
    releaseAll();

    source_ = &source;
    rebindAll();
}

void TablePanel::setHistorySeconds(double seconds)
{
    if (seconds == history_seconds_)
    {
        return;
    }
    history_seconds_ = seconds;

    // The buffers carry their retention at construction, so this rebuilds them
    // and discards what they had collected. Honest rather than convenient: a
    // buffer cannot grow a past it never recorded.
    rebindAll();
}

bool TablePanel::acceptsBinding(const BindingCandidate& candidate) const
{
    // The same test a plot makes, deliberately: anything an expression can turn
    // into a number, which INCLUDES enums and bools -- they are the fields this
    // panel reads better than a plot does. A topic-level candidate belongs to
    // some other kind of panel.
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

bool TablePanel::addBinding(const BindingCandidate& candidate)
{
    if (!acceptsBinding(candidate))
    {
        return false;
    }

    const auto schema =
        reflection::enum_traits<pub_sub::schema_type_t>::try_from_string(candidate.schema_name);
    if (!schema)
    {
        SPDLOG_WARN("Panel '{}': schema '{}' is not in the registry.", cfg_.title,
                    candidate.schema_name);
        return false;
    }

    table_row_t binding;
    binding.zenoh_key = candidate.zenoh_key;
    binding.schema_type = *schema;
    // The degenerate expression: just read the field. Editable afterwards.
    binding.value_expression = candidate.defaultExpression();
    binding.label = candidate.field_name;

    // Already listed? A second identical row reads out the same number twice and
    // doubles the decode cost for nothing.
    const auto duplicate =
        std::find_if(cfg_.rows.begin(), cfg_.rows.end(), [&binding](const table_row_t& existing) {
            return existing.zenoh_key == binding.zenoh_key &&
                   existing.schema_type == binding.schema_type &&
                   existing.value_expression == binding.value_expression;
        });
    if (duplicate != cfg_.rows.end())
    {
        return false;
    }

    cfg_.rows.push_back(binding);
    syncRows();
    emit configChanged();
    return true;
}

std::vector<QString> TablePanel::bindingLabels() const
{
    std::vector<QString> labels;
    labels.reserve(rows_.size());
    for (const std::unique_ptr<Row>& row : rows_)
    {
        labels.push_back(row->displayLabel());
    }
    return labels;
}

std::size_t TablePanel::unboundBindingCount() const
{
    return static_cast<std::size_t>(std::count_if(rows_.begin(), rows_.end(),
                                                  [](const std::unique_ptr<Row>& row)
                                                  { return !row->bound; }));
}

bool TablePanel::removeBinding(std::size_t index)
{
    if (index >= cfg_.rows.size())
    {
        return false;
    }
    cfg_.rows.erase(cfg_.rows.begin() + static_cast<std::ptrdiff_t>(index));
    syncRows();
    emit configChanged();
    return true;
}

void TablePanel::setTimeBase(TimeBase* time_base)
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

    connect(time_base_, &TimeBase::frame, this, &TablePanel::onFrame);
    connect(time_base_, &TimeBase::changed, this, [this]() { update(); });

    // The cursor moving is a repaint for this panel and not merely a decoration:
    // under a cursor every cell reads out that instant, so a table that ignored
    // cursorMoved would sit showing the previous one while the plot beside it
    // moved.
    connect(time_base_, &TimeBase::cursorMoved, this, [this]() { update(); });
}

QString TablePanel::title() const
{
    return QString::fromStdString(cfg_.title);
}

void TablePanel::onFrame()
{
    if (time_base_ == nullptr)
    {
        return;
    }

    // Draining is what moves samples from the producer's ring into the history
    // the readout reads. It happens even while paused: the view freezes, the
    // data does not.
    const double now = time_base_->source().now();
    std::size_t moved = 0;
    for (const std::unique_ptr<Row>& row : rows_)
    {
        moved += row->buffer->drain(now);
    }

    // Same rule as the plot: repaint when samples arrived or the readout
    // instant moved (the age column also ages against now, so a live tick
    // always repaints -- now advances). A paused review with nothing arriving
    // redraws nothing.
    const double readout = now;
    const std::optional<double> cursor = time_base_->cursor();
    const bool instant_moved = readout != last_frame_now_ || cursor != last_frame_cursor_;
    last_frame_now_ = readout;
    last_frame_cursor_ = cursor;

    if (moved > 0 || instant_moved)
    {
        update();
    }
}

// ------------------------------------------------------------------- readings

bool TablePanel::readingAtCursor() const
{
    return cfg_.follow_cursor && time_base_ != nullptr && time_base_->cursor().has_value();
}

double TablePanel::readoutTime() const
{
    if (time_base_ == nullptr)
    {
        // No window around this panel -- a test, or a panel not yet installed.
        // The source's clock is the only instant there is.
        return source_->now();
    }
    if (readingAtCursor())
    {
        return *time_base_->cursor();
    }
    return time_base_->viewEnd();
}

TablePanel::Reading TablePanel::readAt(const SignalBuffer& buffer, double t)
{
    Reading reading;

    const SampleHistory& history = buffer.history();
    if (history.empty())
    {
        return reading;
    }

    // lowerBound() is the first sample at or after `t`. Take it when it lands
    // exactly on the instant, and otherwise the one before -- the newest sample
    // that had already happened.
    const std::size_t at = history.lowerBound(t);
    std::size_t index = 0;
    if (at < history.size() && history[at].t == t)
    {
        index = at;
    }
    else if (at > 0)
    {
        index = at - 1;
    }
    else
    {
        // Every sample is newer than the readout instant: the cursor is parked
        // before this signal started. Nothing to show, which is a different
        // thing from zero.
        return reading;
    }

    reading.has_value = true;
    reading.value = history[index].v;
    reading.sample_t = history[index].t;
    reading.age = std::max(0.0, t - reading.sample_t);
    return reading;
}

QString TablePanel::formatCell(const Row& row, double value) const
{
    if (row.as_state)
    {
        return row.states.label(value);
    }
    if (row.binding.format == cell_format_t::hex)
    {
        // Rounded first: a bitmask is an integer, and 0x3.8 is not a thing
        // anyone wants to read. Negative values print with the sign rather than
        // as a two's-complement pattern of a width nothing here declares.
        const auto integral = static_cast<long long>(std::llround(value));
        const QString digits = QString::number(std::abs(integral), 16).toUpper();
        return (integral < 0 ? QStringLiteral("-0x") : QStringLiteral("0x")) + digits;
    }
    return formatValue(value, row.binding.decimals);
}

// -------------------------------------------------------------------- geometry

double TablePanel::rowHeight() const
{
    const QFontMetricsF metrics(font());
    return metrics.height() + kRowPad;
}

TablePanel::Columns TablePanel::columns() const
{
    Columns out;

    const double width_px = static_cast<double>(width());

    // A width the user chose still has to fit in the panel they are looking at.
    // Clamping here rather than in the drag is what keeps a column honest across
    // a RESIZE: the drag can only know the panel it happened in, and a dock
    // dragged narrow afterwards would otherwise push the name column off the
    // left edge and leave a readout with no signal names in it.
    const double ceiling = std::max(kMinColumnWidth, width_px * 0.4);
    const auto resolve = [ceiling](double configured, double fallback) {
        return std::clamp(configured >= 0.0 ? configured : fallback, kMinColumnWidth, ceiling);
    };

    // From the right. The three sized columns pack against the right edge and
    // the name column absorbs the slack, which is what makes a divider drag mean
    // one thing: it moves its own column's left edge.
    out.age_width = cfg_.show_age ? resolve(cfg_.age_width, kAgeWidth) : 0.0;
    out.units_width = cfg_.show_units ? resolve(cfg_.units_width, kUnitsWidth) : 0.0;

    const double age_right = width_px - kPad;
    out.age_left = age_right - out.age_width;

    const double units_right = out.age_left - (out.age_width > 0.0 ? kColumnGap : 0.0);
    out.units_left = units_right - out.units_width;

    // From units_LEFT, not units_right. A hidden units column has zero width, so
    // its left and right edges coincide and this correctly subtracts no gap.
    const double value_right = out.units_left - (out.units_width > 0.0 ? kColumnGap : 0.0);

    out.name_left = kPad;

    // What the name and value columns share between them, after the fixed ones.
    const double usable = std::max(0.0, value_right - out.name_left - kColumnGap);

    // Automatic: a share of the panel, so a wide dock spells "airplayHandshake"
    // in full. Explicit: exactly what was dragged.
    const double wanted =
        cfg_.value_width >= 0.0
            ? cfg_.value_width
            : std::clamp(usable * kValueShare, std::min(kMinValueWidth, usable), kMaxValueWidth);

    // The name column keeps its minimum even against an explicit width. A value
    // column that ate the whole panel would leave rows that are readings with
    // nothing saying what they are readings OF -- which is worse than a value
    // that elides, because an elided number still shows it is a number.
    out.value_width = std::clamp(wanted, 0.0, std::max(0.0, usable - kMinNameWidth));
    out.value_left = value_right - out.value_width;

    out.name_width = std::max(kMinNameWidth, out.value_left - kColumnGap - out.name_left);
    return out;
}

// ------------------------------------------------------------------- gestures

TablePanel::Divider TablePanel::dividerAt(double x) const
{
    const Columns columns = this->columns();

    // The line sits in the middle of the gap before each column, which is where
    // it is drawn. Tested right to left so that two columns squeezed against
    // each other resolve to the rightmost -- the one with room to grow left.
    const auto near = [x](double edge) {
        return std::abs(x - (edge - kColumnGap * 0.5)) <= kGrabTolerance;
    };

    if (cfg_.show_age && near(columns.age_left))
    {
        return Divider::Age;
    }
    if (cfg_.show_units && near(columns.units_left))
    {
        return Divider::Units;
    }
    if (near(columns.value_left))
    {
        return Divider::Value;
    }
    return Divider::None;
}

void TablePanel::resizeColumn(Divider which, double x)
{
    const Columns before = columns();
    const double grab = x + kColumnGap * 0.5;  // Undo the offset dividerAt() applies.

    // Each column's RIGHT edge is fixed by whatever is to its right, so a drag
    // is simply "how much is left between the pointer and that edge".
    double* target = nullptr;
    double right_edge = 0.0;

    switch (which)
    {
        case Divider::Value:
            target = &cfg_.value_width;
            right_edge = before.value_left + before.value_width;
            break;
        case Divider::Units:
            target = &cfg_.units_width;
            right_edge = before.units_left + before.units_width;
            break;
        case Divider::Age:
            target = &cfg_.age_width;
            right_edge = before.age_left + before.age_width;
            break;
        case Divider::None:
            return;
    }

    *target = std::clamp(right_edge - grab, kMinColumnWidth, kMaxColumnWidth);

    // Then store back WHAT THE PANEL WILL ACTUALLY SHOW. columns() clamps a
    // width against the space there is -- the 40% ceiling, the name column's
    // minimum -- and without this second step a drag past one of those limits
    // would save a number the panel never drew. The workspace would then
    // describe a layout nobody has ever seen, and reloading it would appear to
    // change the widths by itself.
    const Columns shown = columns();
    switch (which)
    {
        case Divider::Value:
            *target = shown.value_width;
            break;
        case Divider::Units:
            *target = shown.units_width;
            break;
        case Divider::Age:
            *target = shown.age_width;
            break;
        case Divider::None:
            break;
    }
    *target = std::clamp(*target, kMinColumnWidth, kMaxColumnWidth);

    update();
}

void TablePanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        // Right-click belongs to the context menu, which is how a signal is
        // added and removed.
        event->ignore();
        return;
    }

    dragging_ = dividerAt(event->position().x());
    if (dragging_ == Divider::None)
    {
        event->ignore();
        return;
    }
    event->accept();
}

void TablePanel::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_ != Divider::None)
    {
        resizeColumn(dragging_, event->position().x());
        event->accept();
        return;
    }

    // Hover feedback. Without it the dividers are invisible affordances: there
    // is no header chrome to suggest a column can be resized, so the cursor is
    // the only thing that says so before the user tries.
    //
    // NOTE the plain QCursor call. TimeSeriesPanel has a setCursor(optional<
    // double>) of its own that silently hides QWidget::setCursor -- a cursor
    // SHAPE converts to double through int and parks the shared time cursor at
    // "13 seconds", compiling cleanly. This panel deliberately has no such
    // overload, and should not grow one.
    setCursor(dividerAt(event->position().x()) == Divider::None ? Qt::ArrowCursor
                                                                : Qt::SplitHCursor);
    event->ignore();
}

void TablePanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragging_ == Divider::None)
    {
        event->ignore();
        return;
    }

    dragging_ = Divider::None;

    // ONCE, at the end of the gesture. Emitting per mouse-move would mark the
    // workspace dirty sixty times a second and, on any listener that does real
    // work, turn a smooth drag into a stutter proportional to how much is bound.
    emit configChanged();
    event->accept();
}

void TablePanel::mouseDoubleClickEvent(QMouseEvent* event)
{
    const Divider which = dividerAt(event->position().x());
    if (event->button() != Qt::LeftButton || which == Divider::None)
    {
        event->ignore();
        return;
    }

    // Back to automatic. THE WAY OUT OF A BAD DRAG, and it needs to exist: a
    // column dragged to 24 px is saved in the workspace, so without this the
    // only way back is to hand-edit the YAML -- and the sentinel that means
    // "size it yourself" is not something a user would guess.
    switch (which)
    {
        case Divider::Value:
            cfg_.value_width = -1.0;
            break;
        case Divider::Units:
            cfg_.units_width = -1.0;
            break;
        case Divider::Age:
            cfg_.age_width = -1.0;
            break;
        case Divider::None:
            return;
    }

    dragging_ = Divider::None;
    update();
    emit configChanged();
    event->accept();
}

void TablePanel::leaveEvent(QEvent* event)
{
    // Or the split cursor follows the pointer out of the panel and onto
    // whatever is beside it.
    setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(event);
}

void TablePanel::clampScroll()
{
    const double body = static_cast<double>(height()) - kPad - rowHeight();
    const int visible = std::max(1, static_cast<int>(body / rowHeight()));
    const int max_start = std::max(0, static_cast<int>(rows_.size()) - visible);
    scroll_row_ = std::clamp(scroll_row_, 0, max_start);
}

void TablePanel::wheelEvent(QWheelEvent* event)
{
    if (rows_.empty())
    {
        event->ignore();
        return;
    }

    // One notch is 120 eighths of a degree by Qt's convention; three rows a
    // notch is the platform default for a list.
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0)
    {
        event->ignore();
        return;
    }

    const int before = scroll_row_;
    scroll_row_ -= notches * 3;
    clampScroll();

    if (scroll_row_ != before)
    {
        update();
    }

    // Accepted either way. A table that let an unusable scroll fall through to
    // the dock would hand the gesture to whatever is underneath, and on a
    // tabified dock that is another panel entirely.
    event->accept();
}

// --------------------------------------------------------------------- drawing

void TablePanel::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);

    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    if (rows_.empty())
    {
        painter.setPen(kEmptyHint);
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No signals.\nDrag one here from the browser."));
        return;
    }

    const QFontMetricsF metrics(painter.font());
    const double line = rowHeight();
    const double width_px = static_cast<double>(width());

    const Columns columns = this->columns();

    // ---- header

    double y = kPad;
    painter.setPen(kHeaderText);
    painter.drawText(QRectF(columns.name_left, y, columns.name_width, line),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("Signal"));
    painter.drawText(QRectF(columns.value_left, y, columns.value_width, line),
                     Qt::AlignRight | Qt::AlignVCenter, tr("Value"));
    if (cfg_.show_age)
    {
        painter.drawText(QRectF(columns.age_left, y, columns.age_width, line),
                         Qt::AlignRight | Qt::AlignVCenter, tr("Age"));
    }

    y += line;
    painter.setPen(QPen(kRule, 1.0));
    painter.drawLine(QPointF(kPad, y), QPointF(width_px - kPad, y));

    // ---- the dividers themselves
    //
    // Faint, full height, and drawn BEFORE the rows so text sits on top of them.
    // They are not decoration: a divider that can be dragged and cannot be seen
    // is an affordance nobody finds, and the hover cursor only helps a user
    // already sweeping the pointer across the exact pixel. Kept at the grid
    // colour so a readout still reads as a readout rather than as a spreadsheet.
    const double body_top = y;
    const double body_bottom = static_cast<double>(height()) - kPad;
    if (body_bottom > body_top)
    {
        painter.setPen(QPen(kRule, 1.0));
        const auto rule = [&](double left) {
            const double x = left - kColumnGap * 0.5;
            painter.drawLine(QPointF(x, body_top), QPointF(x, body_bottom));
        };
        rule(columns.value_left);
        if (cfg_.show_units)
        {
            rule(columns.units_left);
        }
        if (cfg_.show_age)
        {
            rule(columns.age_left);
        }
    }

    // ---- rows

    const double t = readoutTime();

    // Recomputed here rather than in resizeEvent: a dock resize and a row
    // removal both change what fits, and one place that answers "what is
    // visible" cannot disagree with itself.
    clampScroll();

    int drawn = 0;
    for (std::size_t i = static_cast<std::size_t>(scroll_row_); i < rows_.size(); ++i)
    {
        if (y + line > static_cast<double>(height()))
        {
            break;
        }

        const Row& row = *rows_[i];
        const QRectF name_rect(columns.name_left, y, columns.name_width, line);
        const QRectF value_rect(columns.value_left, y, columns.value_width, line);

        painter.setPen(row.bound ? kName : kUnbound);
        painter.drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(row.displayLabel(), Qt::ElideRight,
                                            columns.name_width));

        if (!row.bound)
        {
            painter.setPen(kUnbound);
            painter.drawText(value_rect, Qt::AlignRight | Qt::AlignVCenter, tr("unbound"));
            y += line;
            ++drawn;
            continue;
        }

        const Reading reading = readAt(*row.buffer, t);
        if (!reading.has_value)
        {
            // Nothing at or before this instant. An em dash rather than a zero,
            // which is a reading somebody could act on.
            painter.setPen(kHeaderText);
            painter.drawText(value_rect, Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("--"));
            y += line;
            ++drawn;
            continue;
        }

        const bool stale = reading.age > cfg_.stale_seconds;

        painter.setPen(stale ? kStale : kValue);
        painter.drawText(value_rect, Qt::AlignRight | Qt::AlignVCenter,
                         metrics.elidedText(formatCell(row, reading.value), Qt::ElideRight,
                                            columns.value_width));

        if (cfg_.show_units && !row.binding.units.empty())
        {
            painter.setPen(kHeaderText);
            painter.drawText(QRectF(columns.units_left, y, columns.units_width, line),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             metrics.elidedText(QString::fromStdString(row.binding.units),
                                                Qt::ElideRight, columns.units_width));
        }

        if (cfg_.show_age)
        {
            painter.setPen(stale ? kStale : kHeaderText);
            painter.drawText(QRectF(columns.age_left, y, columns.age_width, line),
                             Qt::AlignRight | Qt::AlignVCenter, formatAge(reading.age));
        }

        y += line;
        ++drawn;
    }

    // Rows that did not fit. Said rather than silently clipped: a panel showing
    // eight of forty signals, with no indication of the other thirty-two, is a
    // readout that is wrong by omission -- and the wheel that reaches them is
    // not discoverable without this.
    const int remaining = static_cast<int>(rows_.size()) - scroll_row_ - drawn;
    if (remaining > 0 || scroll_row_ > 0)
    {
        painter.setPen(kEmptyHint);
        painter.drawText(QRectF(columns.name_left, static_cast<double>(height()) - line,
                                width_px - 2.0 * kPad, line),
                         Qt::AlignRight | Qt::AlignVCenter,
                         tr("%1 more — scroll").arg(remaining + scroll_row_));
    }
}

// ---------------------------------------------------------------------- stats

TablePanel::stats_t TablePanel::stats() const
{
    stats_t all;
    all.readout_t = readoutTime();
    all.at_cursor = readingAtCursor();
    all.rows.reserve(rows_.size());

    for (const std::unique_ptr<Row>& row : rows_)
    {
        row_stats_t stats;
        stats.label = row->displayLabel().toStdString();
        stats.bound = row->bound;
        stats.state = row->as_state;
        stats.received = row->buffer->received();
        stats.dropped = row->buffer->dropped();
        stats.retained = row->buffer->history().size();

        const Reading reading = readAt(*row->buffer, all.readout_t);
        if (reading.has_value)
        {
            stats.has_value = true;
            stats.value = reading.value;
            stats.text = formatCell(*row, reading.value).toStdString();
            stats.sample_t = reading.sample_t;
            stats.age_seconds = reading.age;
            stats.stale = reading.age > cfg_.stale_seconds;
        }

        all.rows.push_back(std::move(stats));
    }

    return all;
}

}  // namespace scope

#include "table/moc_table_panel.cpp"
