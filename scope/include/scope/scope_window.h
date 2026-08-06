#ifndef SCOPE_SCOPE_WINDOW_H_
#define SCOPE_SCOPE_WINDOW_H_

#include "scope/panel_registry.h"
#include "scope/panel_types.h"
#include "scope/workspace.h"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <vector>

class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QDoubleSpinBox;
class QSlider;
class QToolButton;

namespace scope
{

class DataSource;
class LiveZenohSource;
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

    // Review a recording on disk: a bag DIRECTORY, from `bag record` or from
    // scope's own Save Recording. Dialog-free, per the layer split above.
    //
    // False when the directory is not a readable bag, leaving the current
    // source untouched -- a failed open must not drop the window into a review
    // of nothing.
    bool openRecording(const QString& directory);

    // The wrapper. False when the user cancelled.
    bool openRecordingDialog();

    // Back to the live bus, whatever was being reviewed.
    void goLive();

    // Is the current source a recording rather than the bus?
    bool isReviewing() const;

    // ------------------------------------------------------------- capture

    // Review scope's OWN capture -- what it has been recording off the bus
    // since it started. False when there is nothing captured yet.
    //
    // Capture KEEPS RUNNING while it is being reviewed. Stopping it would mean
    // that deciding to look at something costs you everything that happened
    // while you looked, which is the wrong way round.
    bool reviewCapture();

    // Write the capture out as an ordinary bag directory. Dialog-free.
    bool saveCaptureTo(const QString& directory);

    // The wrapper. False when the user cancelled.
    bool saveCaptureDialog();

    ScopeRecorder* recorder() { return recorder_.get(); }

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

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void buildMenuBar();
    void showPanelMenu(const QString& panel_id, const QPoint& at);
    void buildTransportBar();

    // Show the control set the current source's caps() call for, and hide the
    // other. Both are built once, at startup, because a toolbar rebuilt on
    // every source change would lose the object names the agent interface
    // addresses -- and re-creating widgets to change their visibility is how a
    // toolbar ends up with two Pause buttons.
    void applySourceCaps();

    // Per-frame refresh of the scrubber and the position readout.
    void updateTransport();

    void updateEmptyHint();
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

    // The two control sets. QToolBar::addWidget hands back the QAction that
    // owns the widget's place in the bar, and hiding that is what removes the
    // widget AND the separator space around it -- hiding the widget alone
    // leaves a gap where it was.
    std::vector<QAction*> live_controls_;
    std::vector<QAction*> review_controls_;

    QToolButton* play_button_ = nullptr;
    QComboBox* rate_combo_ = nullptr;
    QSlider* scrubber_ = nullptr;
    QLabel* transport_status_ = nullptr;

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
