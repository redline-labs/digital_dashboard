#ifndef SCOPE_SCOPE_WINDOW_H_
#define SCOPE_SCOPE_WINDOW_H_

#include "scope/panel_registry.h"
#include "scope/panel_types.h"
#include "scope/workspace.h"

#include <QMainWindow>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QDoubleSpinBox;
class QToolButton;

namespace scope
{

class DataSource;
class LiveZenohSource;
class OverviewStrip;
class Panel;
class ScopeRecorder;
class SignalBrowser;
class TimeBase;

// The scope's top-level window.
//
// A QMainWindow rather than a plain QWidget because panels are QDockWidgets:
// the user composes the window by docking, tabbing, splitting and floating
// them, and QMainWindow is what implements all of that.
//
// The consequence to remember is that every dock MUST have an objectName.
// restoreState() silently drops any dock it cannot name, so a workspace would
// come back missing panels with nothing logged -- which is why addPanel()
// assigns one and nothing else is allowed to.
class ScopeWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit ScopeWindow(QWidget* parent = nullptr);
    ~ScopeWindow() override;

    // A panel and the dock that holds it, paired because everything that
    // addresses a panel from outside -- the agent interface, the workspace
    // codec -- needs both.
    struct PanelEntry
    {
        QString id;
        Panel* panel = nullptr;
        QDockWidget* dock = nullptr;
    };

    // Adds a panel of `type` and returns its id, or an empty string when the
    // type is unknown. `id` is generated when empty.
    QString addPanel(panel_type_t type, const QString& id = QString());

    // Adds a panel from an existing configuration -- what loading a workspace
    // does.
    QString addPanelFromConfig(const panel_config_variant_t& config, const QString& id);

    bool removePanel(const QString& id);

    const std::vector<PanelEntry>& panels() const { return panels_; }
    PanelEntry* findPanel(const QString& id);

    TimeBase& timeBase() { return *time_base_; }
    DataSource& source();
    SignalBrowser* browser() { return browser_; }

    // Swap the whole source out: into review over a recording, or back to live.
    //
    // Performs the sequence, and the sequence is the point. Panels rebind
    // FIRST, while the old source is still alive and can honour the releases;
    // the browser and the time base follow; only then is the old source
    // destroyed. Nothing above here knows which kind it now has -- that is what
    // the DataSource seam was for.
    void setSource(std::unique_ptr<DataSource> source);

    // Open a recording on disk: a bag DIRECTORY, from `bag record` or from
    // scope's own Save Recording. Dialog-free, per the layer split above.
    //
    // Puts the window OFFLINE, because a bag is an offline source -- and stops
    // the capture for the same reason going offline does. The captured buffer
    // is kept, so Review Session Capture can still reach it.
    //
    // False when the directory is not a readable bag, leaving the current
    // source untouched -- a failed open must not drop the window into a review
    // of nothing.
    bool openRecording(const QString& directory);

    // The wrapper. False when the user cancelled.
    bool openRecordingDialog();

    // ---------------------------------------------------------------- mode
    //
    // ONE bit of state, derived from the source and never stored: a window is
    // online exactly when its source is tailing the bus. Storing it separately
    // is how a mode indicator ends up disagreeing with what is behind the
    // panels after an open that failed.

    // Attach to the live bus and start a FRESH capture.
    //
    // False when the user declined at the unsaved-capture prompt, which is the
    // one thing that can refuse: starting a new session discards the previous
    // capture, and a capture cannot be re-made.
    bool goOnline();

    // Detach from the bus and stop capturing.
    //
    // Lands on the session capture when there is one, because leaving online is
    // almost always "let me look at what just happened"; on an EmptySource when
    // there is not. The capture's buffer OUTLIVES the recorder's subscriber --
    // see ScopeRecorder::stop() -- which is what makes landing on it safe.
    void goOffline();

    // Is the source tailing the bus?
    bool isOnline() const;

    // ------------------------------------------------------------- capture

    // Review scope's OWN capture -- what it recorded off the bus during the
    // online session. False when there is nothing captured yet.
    //
    // The capture is a SNAPSHOT, not a tail: it runs only while the window is
    // online, so reviewing it puts the window offline and the buffer stops
    // growing. The alternative -- keeping the subscriber alive so the capture
    // grows while you scrub it -- was rejected because it makes "offline" a
    // claim the process does not honour: an offline window would still be on the
    // bus. The cost is real and worth stating: the interval you spend looking is
    // not captured, so a rare event you go back online to catch can be missed.
    bool reviewCapture();

    // Is there a capture with anything in it? What the Review and Save
    // Recording actions are enabled from, so a button that cannot work is
    // disabled rather than silently doing nothing.
    bool hasCapture() const;

    // Write the capture out as an ordinary bag directory. Dialog-free.
    bool saveCaptureTo(const QString& directory);

    // The wrapper. False when the user cancelled.
    bool saveCaptureDialog();

    ScopeRecorder* recorder() { return recorder_.get(); }

    // Messages per uniform bucket over [begin, end] on the source's clock.
    //
    // Goes through the window rather than straight to the source because a LIVE
    // source cannot answer -- it keeps no history of its own -- and the honest
    // answer comes from the recorder, which has been capturing the whole bus
    // since startup. The window is the only thing holding both, and the two
    // clocks need reconciling. Doing it here rather than at each caller is what
    // keeps `scope.density` reporting the same numbers the overview strip is
    // drawing; the RPC exists precisely to check the strip, so the two
    // disagreeing would make it useless.
    //
    // False means nothing could answer cheaply, which the strip draws as a plain
    // band. NOT per frame -- see CaptureBuffer::density().
    bool densityFor(double begin, double end, std::size_t buckets,
                    std::vector<std::uint32_t>& out);

  signals:
    // The source was replaced. The transport bar rebuilds from caps().
    void sourceChanged();

  public:

    const QString& workspacePath() const { return workspace_path_; }
    void setWorkspacePath(QString path) { workspace_path_ = std::move(path); }

    // Dock arrangement as an opaque, Qt-versioned blob.
    //
    // Stored ALONGSIDE the readable YAML, never instead of it. Everything that
    // matters semantically -- which panels exist, what each plots, how it is
    // styled -- is in the YAML and is editable by hand. This carries only the
    // arrangement, so losing it to a Qt upgrade costs a re-drag, not data. That
    // is why restore failure is a warning and a default layout rather than an
    // error.
    QByteArray dockState() const;
    bool restoreDockState(const QByteArray& state);

    // Replaces everything: panels, time base, arrangement.
    //
    // These two are the DIALOG-FREE layer, and everything that is not a menu
    // item goes through them: the agent interface, --config at startup, the
    // tests. The menu items are thin wrappers that pick a path and then call
    // these. Keeping the split means a headless run never reaches a modal
    // QFileDialog, which has no one to dismiss it and hangs the process.
    bool loadWorkspace(const QString& path);
    bool saveWorkspace(const QString& path);

    // The wrappers. False when the user cancelled, which is not an error.
    bool openWorkspaceDialog();
    bool saveWorkspaceDialog();

    // The current state as a workspace, without writing it anywhere.
    scope_workspace_t toWorkspace() const;

    // Seconds of samples each bound signal retains. Workspace-level rather than
    // per-panel: two panels plotting the same signal must not disagree about how
    // far back it goes.
    double historySeconds() const { return history_seconds_; }
    void setHistorySeconds(double seconds);

    // No modal dialog may be raised while this is set, because there is nobody
    // to dismiss it -- under --mcp the window is driven headlessly and a
    // QFileDialog or a QMessageBox would hang the process with no diagnostic at
    // all. Set once from main(), from the same flag that chooses the offscreen
    // platform.
    void setHeadless(bool headless) { headless_ = headless; }
    bool isHeadless() const { return headless_; }

    // Does the window hold state that is not on disk?
    bool isDirty() const { return dirty_; }
    void markDirty();
    void markClean();

    // False when the user wants to keep what they have, so the caller must
    // abandon whatever it was about to do. True when there is nothing to lose --
    // and true, with a warning, when there is nobody to ask.
    bool confirmDiscardChanges(const QString& action);

    // The capture half of the above, on its own, because going online destroys
    // a capture without touching the workspace. Asking about the workspace there
    // too would train the user to dismiss a prompt that means something else.
    bool confirmDiscardCapture(const QString& action);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void buildMenuBar();

    // The central area: the offline landing screen and the "no panels yet"
    // hint, which are two different messages and used to be one label.
    //
    // "Offline with nothing loaded" is a state worth explaining rather than
    // merely indicating, because it is now the state every window starts in.
    // A hint that only said "No panels yet" left the mode unmentioned on the
    // one screen where it decides what the user should do next.
    void buildCentralArea();

    void showPanelMenu(const QString& panel_id, const QPoint& at);
    void buildTransportBar();

    // The top bar: which mode the window is in, what it is made of, and the
    // zoom actions. Everything about the TIME axis stays on the bottom bar with
    // the time axis.
    //
    // It is built from the QAction objects the menus already own rather than
    // from copies. One action means one objectName, one handler and one
    // enabled-state, so the toolbar cannot drift out of step with the menu that
    // does the same thing -- which is exactly what two copies would do the
    // first time one of them grew a guard.
    void buildMainToolBar();

    // Window-level zoom/fit actions, shared by the toolbar buttons and the
    // keyboard. A gesture and a key press must reach the same code or they will
    // disagree about clamping the first time one is changed.
    void buildNavigationActions();

    // Show the control set the current source's caps() call for, and hide the
    // other. Both are built once, at startup, because a toolbar rebuilt on
    // every source change would lose the object names the agent interface
    // addresses -- and re-creating widgets to change their visibility is how a
    // toolbar ends up with two Pause buttons.
    void applySourceCaps();

    // Per-frame refresh of the overview strip and the position readout.
    void updateTransport();

    // Push the strip's marks -- extent, view, cursor, playhead, retained band.
    // Cheap, so it runs every frame. The histogram behind them does NOT; see
    // refreshDensity().
    void updateOverview();

    // Recompute the strip's background histogram, at most every
    // kDensityIntervalMs and only when something it depends on has moved.
    //
    // THE THROTTLE IS LOAD-BEARING, not a tuning knob. CaptureBuffer::revision()
    // bumps on every push and every eviction -- thousands a second on a busy bus
    // -- so a revision check alone would recompute every frame, walking millions
    // of entries while holding the mutex the zenoh RX thread needs to push. That
    // does not merely cost the consumer; it stalls the producer.
    void refreshDensity();

    void updateEmptyHint();

    // What the top bar's chip says about the current source. Cheap enough to
    // run from applySourceCaps() and from the render tick, because online it
    // has to count a growing capture.
    void updateSourceChip();

    // Enable or disable the actions that need something to act on: reviewing
    // and saving a capture that does not exist yet. A disabled action is how a
    // control says "not now"; the alternative -- staying enabled and doing
    // nothing but a status-bar line -- is what the old Review button did, and
    // it reads as a broken app rather than an unavailable one.
    void updateModeActions();

    void updateWindowTitle();
    QString uniqueId(panel_type_t type) const;

    // The INTERFACE, not the concrete live type. Holding LiveZenohSource here
    // was what blocked recorded playback: everything above this line already
    // spoke DataSource, and only the member's type said otherwise.
    std::unique_ptr<DataSource> source_;

    std::unique_ptr<TimeBase> time_base_;

    // Declared AFTER source_ so it outlives it: a RecordedSource over a
    // CaptureProvider holds a pointer into this recorder's buffer, and members
    // are destroyed in reverse declaration order.
    std::unique_ptr<ScopeRecorder> recorder_;

    // Has anything been captured that is not on disk? Tracked separately from
    // the workspace's dirty flag because they are lost differently: a workspace
    // can be re-made by hand, a capture cannot be re-made at all.
    bool capture_saved_ = false;

    // What the offline source is, for the top bar's chip: a bag's directory
    // name and duration, or empty when the source is the capture or nothing.
    // Held rather than re-derived because a RecordedSource does not know where
    // it came from -- it has a provider, and a CaptureProvider has no path at
    // all.
    QString source_label_;

    std::uint64_t capture_max_bytes_ = 0;
    double capture_max_seconds_ = 0.0;

    std::vector<PanelEntry> panels_;
    int next_panel_ordinal_ = 1;

    SignalBrowser* browser_ = nullptr;
    QDockWidget* browser_dock_ = nullptr;

    QLabel* empty_hint_ = nullptr;
    QToolButton* pause_button_ = nullptr;
    QDoubleSpinBox* window_spin_ = nullptr;
    QLabel* cursor_label_ = nullptr;

    // The mode control: ONE button for one bit of state, checked when online.
    //
    // Its checked state is set from the SOURCE in applySourceCaps(), never from
    // the click that caused the change -- so a source swapped by the agent
    // interface, by --bag at startup, by an open that failed or by a transition
    // the user cancelled leaves the toolbar saying what is actually behind the
    // panels. A button left tracking its own clicks would be wrong in exactly
    // the cases where being right matters.
    //
    // It replaced a checkable pair (`mode_live` / `mode_review`), which was a
    // hand-rolled radio group for a boolean and left the two halves free to
    // disagree.
    QToolButton* mode_toggle_ = nullptr;

    // What is behind the panels, in words: the bag, the capture, or nothing.
    //
    // NOT the capture's state -- that is transport_status_ on the bottom bar,
    // and this label used to duplicate it verbatim. Two widgets rendering one
    // string from one function is a tell that one of them has no job.
    QLabel* source_chip_ = nullptr;

    // Where the offline landing screen lives, shown when the window is offline
    // with nothing loaded. The one place the mode is explained rather than
    // merely indicated.
    QWidget* empty_panel_ = nullptr;

    // The two control sets. QToolBar::addWidget hands back the QAction that
    // owns the widget's place in the bar, and hiding that is what removes the
    // widget AND the separator space around it -- hiding the widget alone
    // leaves a gap where it was.
    std::vector<QAction*> live_controls_;
    std::vector<QAction*> review_controls_;

    QToolButton* play_button_ = nullptr;
    QComboBox* rate_combo_ = nullptr;
    QLabel* transport_status_ = nullptr;

    // Replaced `transport_scrubber`, a QSlider. The name could not be kept: an
    // agent that clicked it and then set a value would be driving a widget that
    // is not a slider any more, and would fail in a way that looks like a
    // broken app rather than a renamed one.
    OverviewStrip* overview_ = nullptr;

    // Reused across recomputations so a running strip allocates nothing.
    std::vector<std::uint32_t> density_;

    // What the cached histogram was computed from, so refreshDensity() can tell
    // whether anything it depends on has actually moved.
    std::uint64_t density_revision_ = 0;
    int density_buckets_ = 0;
    double density_begin_ = 0.0;
    double density_end_ = 0.0;
    std::int64_t density_computed_at_ms_ = 0;

    // Set while the transport bar is being updated FROM the time base, so the
    // widgets' own valueChanged signals do not turn a refresh into a seek. The
    // scrubber is the one that matters: without this, every frame of playback
    // would set the slider, which would seek to where playback already was, and
    // the two would fight at the render rate.
    bool updating_transport_ = false;

    QString workspace_path_;

    // The name from the workspace, kept separately from windowTitle() because
    // the title also carries the dirty marker and reading it back would make
    // "workspace *" the saved name after one edit.
    QString workspace_name_;

    double history_seconds_ = TimeSeriesPanel::kDefaultHistorySeconds;

    bool headless_ = false;
    bool dirty_ = false;
};

}  // namespace scope

#endif  // SCOPE_SCOPE_WINDOW_H_
