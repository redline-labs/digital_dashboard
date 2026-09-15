#ifndef SCOPE_TABLE_PANEL_H_
#define SCOPE_TABLE_PANEL_H_

#include "scope/panel.h"
#include "scope/panel_types.h"
#include "scope/sample_ring.h"
#include "scope/state_names.h"

#include "table/config.h"
#include "table/stats.h"

#include <QString>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>
#include <optional>

namespace scope
{

class DataSource;
class TimeBase;

// What every bound signal reads RIGHT NOW, one row each.
//
// The third panel, and the one that shows the seam was worth building: it is a
// hundred lines of paint code and a config struct, because the browser, the
// drag plumbing, the Add Signal dialog, the workspace codec, the Panels menu and
// six agent-interface verbs all reached it through SCOPE_PANEL_TABLE without
// learning it exists.
//
// IT TAKES THE SAME CANDIDATES A PLOT DOES -- numeric fields, including enums
// and bools -- rather than being a mirror image the way the video panel is. Two
// panels accepting the same candidate is not a conflict: a drop goes to the
// panel it was dropped on, and browser double-click goes to the first panel that
// will have it, which was already the rule.
//
// TWO THINGS IT DOES THAT A PLOT CANNOT, and they are the reason to want it:
//
//   1. AN ENUM READS AS ITS NAME. A plot draws a state lane and writes the name
//      in the band when the band is wide enough to hold it; at a tight zoom, or
//      on a state that changes quickly, it is a colour and nothing else. A cell
//      always has room for the word.
//   2. IT SAYS HOW OLD THE READING IS. A plot shows a line that stops, which is
//      unmissable. A table shows the dead publisher's last value in exactly the
//      typeface it shows a live one -- so the age column and the stale marking
//      are not decoration, they are what stops this panel from lying.
//
// CUSTOM PAINTED RATHER THAN A QTableWidget, which is the obvious alternative
// and wrong twice over. ScopeWindow installs a CustomContextMenu handler on the
// panel widget itself, so a child view would swallow every right-click and the
// "Add signal" menu would be missing precisely where a user would look for it.
// And a QTableWidget wants setText() per cell per frame, which allocates a
// QString per cell at the render rate to redraw text that mostly has not
// changed. Neither is fatal; together they are more work than drawing rows.
class TablePanel : public Panel
{
    Q_OBJECT

  public:
    using config_t = TablePanelConfig_t;
    using stats_t = TablePanelStats_t;

    static constexpr panel_type_t kPanelType = panel_type_t::table;
    static constexpr std::string_view kFriendlyName = "Table";

    // U+25A4 SQUARE WITH HORIZONTAL FILL -- rows in a box. Same rule as the
    // other two glyphs: it is in the default font on every platform this runs
    // on, which the emoji and the box-drawing alternatives are not, and a
    // missing glyph renders as a blank box that reads as a broken button rather
    // than a plain one. Also distinct from ▣ at a glance, which the more obvious
    // ▦ and ▩ are not.
    static constexpr std::string_view kToolbarGlyph = "▤";

    // A readout needs far less history than a plot: it draws one sample per row.
    // It keeps a window rather than a single value anyway, because the shared
    // cursor may sit anywhere in the view and a one-slot mailbox could not
    // answer for it -- but 60 s is enough for any cursor the view can hold at a
    // sane span, and the workspace's history_seconds overrides it.
    static constexpr double kDefaultHistorySeconds = 60.0;

    // `source` must outlive the panel: it owns the subscriptions this binds.
    TablePanel(const config_t& cfg, DataSource& source,
               double history_seconds = kDefaultHistorySeconds, QWidget* parent = nullptr);
    ~TablePanel() override;

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
    stats_t stats() const;

    double historySeconds() const { return history_seconds_; }

    // Where the columns are, in logical pixels. Public so a test can assert the
    // layout rules directly rather than inferring them from pixels.
    struct Columns
    {
        double name_left = 0.0;
        double name_width = 0.0;
        double value_left = 0.0;
        double value_width = 0.0;

        // Zero width when the column is switched off, which is what makes
        // "hidden" and "narrow" the same case everywhere below.
        double units_left = 0.0;
        double units_width = 0.0;
        double age_left = 0.0;
        double age_width = 0.0;
    };

    // THE ONE PLACE THE LAYOUT IS DECIDED. Paint draws from it and the divider
    // hit test measures against it, so a grab can never land somewhere other
    // than the line the user is looking at -- which is exactly the bug two
    // copies of this arithmetic would produce, and it would look like a
    // mis-aimed mouse rather than like a bug.
    Columns columns() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

    // Rows past the bottom edge are reachable rather than lost. A table is the
    // one panel whose content grows without bound as signals are added -- forty
    // rows in a docked panel is a normal thing to want -- and the wheel is the
    // gesture for it. It does NOT reach the shared time base: scrolling a list
    // is not a view change, and a table that zoomed the window out from under
    // the plots beside it would be astonishing.
    void wheelEvent(QWheelEvent* event) override;

    // Dragging a column divider, and double-clicking one to hand it back to the
    // automatic width.
    //
    // These do not touch the shared time base either. Every gesture on a plot
    // moves the window every panel shares; every gesture here changes this
    // panel's own layout and nothing else, so no seek is ever generated.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Row;

    // Which divider a gesture is on. Named for the column to the RIGHT of the
    // line, because that is the one being resized: the three sized columns are
    // packed against the right edge, so a divider moves its own column's LEFT
    // edge and the name column absorbs the difference.
    enum class Divider
    {
        None,
        Value,
        Units,
        Age,
    };

    // The divider within grab distance of `x`, or None. Only ever returns a
    // column that is actually shown -- a hidden one has no line to grab.
    Divider dividerAt(double x) const;

    // Apply a drag: set `which`'s width so its left edge lands on `x`, clamped
    // to what the panel can show. Writes cfg_, which is what pins a column the
    // user has dragged.
    void resizeColumn(Divider which, double x);

    // What one row shows at one instant.
    struct Reading
    {
        bool has_value = false;
        double value = 0.0;
        double sample_t = 0.0;
        double age = 0.0;
    };

    // Reconcile rows_ with cfg_.rows, keeping the buffer -- and therefore the
    // history -- of every row that is still there. THE DEFAULT PATH for any
    // config change, including adding and removing a signal.
    void syncRows();

    // The wholesale version: drop everything and bind again. Only for the two
    // cases where a buffer genuinely cannot be carried over -- a different
    // source issued the handles, or the retention they were built with changed.
    void rebindAll();

    void releaseAll();

    std::unique_ptr<Row> makeRow(const table_row_t& binding);

    // Re-read the presentation half of a binding: which of `format`'s modes the
    // row is in. Separate from makeRow() because a row that only changed how it
    // PRINTS must not be rebound.
    void applyFormat(Row& row) const;

    void onFrame();

    // THE INSTANT EVERY ROW IS READ AT. The shared cursor when there is one and
    // the config follows it, otherwise the view's right edge -- which on a live
    // source is the source's clock and on a recording is the playhead.
    //
    // One function rather than per row, so a table can never print two cells
    // from two instants. Falls back to the source's clock when there is no time
    // base, which is how a panel built outside a window (a test) still reads.
    double readoutTime() const;
    bool readingAtCursor() const;

    // The newest sample at or before `t`. NEVER interpolated: a reading between
    // two samples is a number nothing published, and for a state it is not even
    // wrong -- it is a fractional ordinal that names nothing. Zero-order hold is
    // what the plot's lanes and its legend already do.
    static Reading readAt(const SignalBuffer& buffer, double t);

    // What the cell prints, applying the row's format to the number.
    QString formatCell(const Row& row, double value) const;

    double rowHeight() const;
    void clampScroll();

    config_t cfg_;

    // A pointer, not a reference: rebindTo() moves every row onto a different
    // source when the window enters review over a recording.
    DataSource* source_;

    TimeBase* time_base_ = nullptr;
    double history_seconds_ = kDefaultHistorySeconds;

    // What the last render tick saw, so onFrame() can skip a repaint when
    // neither the data nor the readout instant moved.
    double last_frame_now_ = 0.0;
    std::optional<double> last_frame_cursor_;

    std::vector<std::unique_ptr<Row>> rows_;

    // First row drawn, so a panel shorter than its content can reach the rest.
    // View state rather than config: where a list is scrolled to is not worth
    // saving in a workspace, and restoring it would put a reader somewhere they
    // did not choose.
    int scroll_row_ = 0;

    // The divider currently being dragged. Unlike the scroll position this is
    // not view state that outlives the gesture -- the WIDTH it produces lives in
    // cfg_, and therefore in the workspace, because a column the user sized is a
    // decision and not a scroll offset.
    Divider dragging_ = Divider::None;
};

}  // namespace scope

#endif  // SCOPE_TABLE_PANEL_H_
