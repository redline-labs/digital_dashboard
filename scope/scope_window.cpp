#include "scope/scope_window.h"

#include "scope/add_signal_dialog.h"
#include "scope/data_source.h"
#include "scope/live_zenoh_source.h"
#include "scope/overview_strip.h"
#include "scope/panel.h"
#include "scope/recorded_source.h"
#include "scope/scope_recorder.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "time_series/time_series_panel.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>

namespace scope
{

namespace
{

// A dock that accepts a dragged candidate on behalf of the panel inside it.
//
// The dock rather than the panel, because the panel does not fill the dock --
// there is a title bar and a margin -- and a drop landing on the chrome should
// still work. QDrag::exec() cannot be driven by the agent interface at all, so
// this same path is what scope.browser_drag exercises directly: it sends the
// DragEnter -> DragMove -> Drop triple straight here, which covers the
// accept/reject logic and the drop handler and leaves only the few lines inside
// exec() untested. Exactly the arrangement editor.palette_drag uses.
class PanelDock : public QDockWidget
{
  public:
    PanelDock(const QString& title, QWidget* parent) : QDockWidget(title, parent)
    {
        setAcceptDrops(true);
    }

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (accepts(event->mimeData()))
        {
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (accepts(event->mimeData()))
        {
            event->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent* event) override
    {
        BindingCandidate candidate;
        if (!decodeCandidate(event->mimeData()->data(kSignalMimeType), candidate))
        {
            return;
        }

        auto* panel = qobject_cast<Panel*>(widget());
        if (panel != nullptr && panel->addBinding(candidate))
        {
            event->acceptProposedAction();
        }
    }

  private:
    // Accepts only what it can both parse and use. The editor's canvas learned
    // this the hard way: it used to accept any drag with text, then throw out
    // of a Qt event handler when the text was not a widget type, which
    // terminated the app. Parse first, ask the panel second, accept last.
    bool accepts(const QMimeData* mime) const
    {
        if (mime == nullptr || !mime->hasFormat(kSignalMimeType))
        {
            return false;
        }

        BindingCandidate candidate;
        if (!decodeCandidate(mime->data(kSignalMimeType), candidate))
        {
            return false;
        }

        const auto* panel = qobject_cast<const Panel*>(widget());
        return panel != nullptr && panel->acceptsBinding(candidate);
    }
};

}  // namespace

ScopeWindow::ScopeWindow(QWidget* parent) : QMainWindow(parent)
{
    setObjectName("ScopeWindow");
    setWindowTitle("Redline Scope");
    resize(1280, 800);

    // Docks may occupy the corners of whichever edges meet there. Without this
    // a left dock and a bottom dock fight over the bottom-left corner and the
    // result depends on which was added first, which makes a restored layout
    // subtly different from the one that was saved.
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    // Nested docks are what make a panel splittable in both directions, which
    // is the whole point of the composable layout.
    setDockNestingEnabled(true);

    source_ = std::make_unique<LiveZenohSource>();
    time_base_ = std::make_unique<TimeBase>(*source_);

    // Capture starts with the window and runs for its whole life. Everything on
    // the bus, with no exclusions: the point is that a signal nobody thought to
    // plot can still be added afterwards, and a filter taken from the panels
    // would only ever record what was already on screen.
    const scope_workspace_t defaults;
    capture_max_bytes_ = defaults.max_capture_bytes;
    capture_max_seconds_ = defaults.max_capture_seconds;
    recorder_ = std::make_unique<ScopeRecorder>(static_cast<std::size_t>(capture_max_bytes_),
                                                capture_max_seconds_);

    empty_hint_ = new QLabel(
        tr("No panels yet.\n\nAdd one from Panels ▸ Add, or press Ctrl+N."), this);
    empty_hint_->setObjectName("empty_hint");
    empty_hint_->setAlignment(Qt::AlignCenter);
    empty_hint_->setStyleSheet("color: palette(mid); font-size: 14px;");
    setCentralWidget(empty_hint_);

    browser_ = new SignalBrowser(*source_, this);
    browser_dock_ = new QDockWidget(tr("Signals"), this);
    browser_dock_->setObjectName("dock_signal_browser");
    browser_dock_->setWidget(browser_);
    addDockWidget(Qt::LeftDockWidgetArea, browser_dock_);

    // Double-clicking a field puts it on the first panel that will take it.
    // The keyboard path to the same thing dragging does -- and the only path
    // when there is no mouse, which is every headless run.
    connect(browser_, &SignalBrowser::candidateActivated, this,
            [this](const BindingCandidate& candidate) {
                for (const PanelEntry& entry : panels_)
                {
                    if (entry.panel->acceptsBinding(candidate) &&
                        entry.panel->addBinding(candidate))
                    {
                        statusBar()->showMessage(
                            tr("Added %1 to %2")
                                .arg(QString::fromStdString(candidate.field_name), entry.id),
                            3000);
                        return;
                    }
                }
                statusBar()->showMessage(tr("No panel accepted that signal."), 3000);
            });

    // The window length and the render rate are both saved in the workspace, so
    // changing either means the file on disk no longer describes the window.
    connect(time_base_.get(), &TimeBase::changed, this, [this]() { markDirty(); });

    buildNavigationActions();
    buildMenuBar();
    buildMainToolBar();
    buildTransportBar();
    updateWindowTitle();

    // An empty window is not dirty: there is nothing in it worth keeping, and
    // prompting on the way out of one would train the user to dismiss the
    // prompt that matters.
    markClean();

    statusBar()->showMessage(tr("Ready"));
}

ScopeWindow::~ScopeWindow()
{
    // Panels release their signal bindings in their destructors, which reach
    // into the source. Tear the docks down while the source is still alive
    // rather than letting member destruction order decide -- source_ is
    // declared before the panel list, so it would otherwise go first.
    for (PanelEntry& entry : panels_)
    {
        delete entry.dock;
    }
    panels_.clear();
}

DataSource& ScopeWindow::source()
{
    return *source_;
}

void ScopeWindow::setSource(std::unique_ptr<DataSource> next)
{
    if (!next || next.get() == source_.get())
    {
        return;
    }

    DataSource& to = *next;

    // ORDER IS THE WHOLE THING HERE.
    //
    // Panels first, while the OLD source is still alive: rebindTo() releases
    // every handle against it before repointing, and a handle means nothing to
    // a source that did not issue it. Moving the unique_ptr first would destroy
    // the old source with its subscriptions still registered -- and for the
    // live source that means zenoh callbacks still running against buffers
    // nobody will drain.
    for (PanelEntry& entry : panels_)
    {
        entry.panel->rebindTo(to);
    }

    browser_->setSource(to);
    time_base_->setSource(to);

    // Only now. This is where the old source is destroyed, and by here nothing
    // holds a handle on it.
    source_ = std::move(next);

    applySourceCaps();
    emit sourceChanged();
}

// ---------------------------------------------------------------------- panels

QString ScopeWindow::uniqueId(panel_type_t type) const
{
    const std::string_view type_name = reflection::enum_traits<panel_type_t>::to_string(type);
    for (int ordinal = next_panel_ordinal_;; ++ordinal)
    {
        const QString candidate =
            QStringLiteral("%1#%2").arg(QString::fromUtf8(type_name.data(),
                                                         static_cast<qsizetype>(type_name.size())))
                .arg(ordinal);
        const bool taken = std::any_of(panels_.begin(), panels_.end(),
                                       [&candidate](const PanelEntry& entry)
                                       { return entry.id == candidate; });
        if (!taken)
        {
            return candidate;
        }
    }
}

QString ScopeWindow::addPanel(panel_type_t type, const QString& id)
{
    return addPanelFromConfig(default_panel_config(type), id);
}

QString ScopeWindow::addPanelFromConfig(const panel_config_variant_t& config, const QString& id)
{
    std::unique_ptr<Panel> panel = createPanel(config, *source_, history_seconds_, nullptr);
    if (!panel)
    {
        SPDLOG_WARN("Refusing to add a panel of unknown type.");
        return {};
    }

    const panel_type_t type = panelTypeOf(config);
    const QString panel_id = id.isEmpty() ? uniqueId(type) : id;
    ++next_panel_ordinal_;

    panel->setTimeBase(time_base_.get());

    auto* dock = new PanelDock(panel->title(), this);
    // MANDATORY. restoreState() silently drops any dock it cannot name, so a
    // workspace would come back missing panels with nothing logged.
    dock->setObjectName(panel_id);
    dock->setWidget(panel.get());

    PanelEntry entry;
    entry.id = panel_id;
    entry.panel = panel.release();  // Owned by the dock from here.
    entry.dock = dock;

    // Right by default, and tabbed with an existing panel rather than shrinking
    // everything: a fifth panel added to a four-way split is unreadable, and
    // splitting is one drag away for anyone who wants it.
    addDockWidget(Qt::RightDockWidgetArea, dock);
    if (!panels_.empty())
    {
        tabifyDockWidget(panels_.back().dock, dock);
        dock->raise();
    }

    // Right-click anywhere on the panel to add or drop a signal. The keyboard-
    // and-mouse path that does not need the browser dock to be visible, and the
    // one that still works when the panel is a floating window.
    entry.panel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(entry.panel, &QWidget::customContextMenuRequested, this,
            [this, panel_id](const QPoint& at) { showPanelMenu(panel_id, at); });

    // Anything that changes what a workspace would save makes it dirty. The
    // panel says so itself rather than the window guessing from the several
    // routes in -- a drop, the context menu, the agent interface, the dialog.
    connect(entry.panel, &Panel::configChanged, this, [this]() { markDirty(); });

    panels_.push_back(entry);
    updateEmptyHint();
    markDirty();
    return panel_id;
}

bool ScopeWindow::removePanel(const QString& id)
{
    const auto found = std::find_if(panels_.begin(), panels_.end(),
                                    [&id](const PanelEntry& entry) { return entry.id == id; });
    if (found == panels_.end())
    {
        return false;
    }

    // Deleting the dock deletes the panel it owns, whose destructor releases
    // its signal bindings.
    delete found->dock;
    panels_.erase(found);
    updateEmptyHint();
    markDirty();
    return true;
}

ScopeWindow::PanelEntry* ScopeWindow::findPanel(const QString& id)
{
    const auto found = std::find_if(panels_.begin(), panels_.end(),
                                    [&id](const PanelEntry& entry) { return entry.id == id; });
    return found == panels_.end() ? nullptr : &*found;
}

void ScopeWindow::showPanelMenu(const QString& panel_id, const QPoint& at)
{
    PanelEntry* entry = findPanel(panel_id);
    if (entry == nullptr)
    {
        return;
    }

    QMenu menu(this);
    menu.setObjectName("panel_context_menu");

    QAction* add = menu.addAction(tr("Add signal…"));
    add->setObjectName("action_panel_add_signal");

    auto* plot = qobject_cast<TimeSeriesPanel*>(entry->panel);
    QMenu* remove_menu = nullptr;
    std::vector<QAction*> remove_actions;
    if (plot != nullptr && !plot->getConfig().traces.empty())
    {
        remove_menu = menu.addMenu(tr("Remove signal"));
        remove_menu->setObjectName("menu_panel_remove_signal");
        for (const signal_binding_t& binding : plot->getConfig().traces)
        {
            const std::string& name =
                binding.label.empty() ? binding.value_expression : binding.label;
            remove_actions.push_back(remove_menu->addAction(QString::fromStdString(name)));
        }
    }

    menu.addSeparator();
    QAction* close = menu.addAction(tr("Close panel"));
    close->setObjectName("action_panel_close");

    QAction* chosen = menu.exec(entry->panel->mapToGlobal(at));
    if (chosen == nullptr)
    {
        return;
    }

    if (chosen == add)
    {
        AddSignalDialog dialog(*source_, *entry->panel, this);
        if (dialog.exec() == QDialog::Accepted)
        {
            entry->panel->addBinding(dialog.selected());
        }
        return;
    }

    if (chosen == close)
    {
        removePanel(panel_id);
        return;
    }

    for (std::size_t i = 0; i < remove_actions.size(); ++i)
    {
        if (chosen == remove_actions[i] && plot != nullptr)
        {
            plot->removeSignal(i);
            return;
        }
    }
}

void ScopeWindow::updateEmptyHint()
{
    if (empty_hint_ != nullptr)
    {
        empty_hint_->setVisible(panels_.empty());
    }
}

// ------------------------------------------------------------------- dock state

QByteArray ScopeWindow::dockState() const
{
    return saveState();
}

bool ScopeWindow::restoreDockState(const QByteArray& state)
{
    if (state.isEmpty())
    {
        return false;
    }
    return restoreState(state);
}

// ------------------------------------------------------------------- workspace

scope_workspace_t ScopeWindow::toWorkspace() const
{
    scope_workspace_t workspace;

    // The stored name, not windowTitle(): the title carries the dirty marker,
    // so reading it back would make "my workspace *" the saved name after one
    // edit and "my workspace * *" after the next.
    workspace.name = workspace_name_.toStdString();
    workspace.history_seconds = history_seconds_;
    workspace.max_capture_bytes = capture_max_bytes_;
    workspace.max_capture_seconds = capture_max_seconds_;
    workspace.window_seconds = time_base_->windowSeconds();
    workspace.render_rate_hz = static_cast<uint16_t>(time_base_->renderRateHz());

    for (const PanelEntry& entry : panels_)
    {
        panel_entry_t saved;
        saved.id = entry.id.toStdString();
        saved.type = entry.panel->panelType();

        // Through the table, never a cast to one panel kind. This used to be a
        // qobject_cast<TimeSeriesPanel*> with no else, so any other panel type
        // saved its `type:` with its config left on monostate -- which the YAML
        // encoder then omits entirely, so the panel came back default
        // constructed with nothing logged. Every setting lost on every save.
        saved.config = panelConfigOf(*entry.panel);

        workspace.panels.push_back(std::move(saved));
    }

    workspace.dock_state = saveState().toBase64().toStdString();
    return workspace;
}

bool ScopeWindow::saveWorkspace(const QString& path)
{
    if (!save_workspace(toWorkspace(), path.toStdString()))
    {
        return false;
    }
    workspace_path_ = path;
    markClean();
    statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
    return true;
}

bool ScopeWindow::loadWorkspace(const QString& path)
{
    const std::optional<scope_workspace_t> workspace = load_workspace(path.toStdString());
    if (!workspace)
    {
        return false;
    }

    // Everything goes, including panels that were never saved. A load that
    // merged would leave the window in a state no file describes.
    while (!panels_.empty())
    {
        removePanel(panels_.front().id);
    }

    // BEFORE the panels, because a panel builds its buffers while binding and
    // retention cannot be changed afterwards without discarding them.
    setHistorySeconds(workspace->history_seconds);

    // In place on the existing buffer, not by rebuilding the recorder: capture
    // started with the window, so a rebuild would discard everything recorded
    // before the workspace was opened.
    capture_max_bytes_ = workspace->max_capture_bytes;
    capture_max_seconds_ = workspace->max_capture_seconds;
    if (recorder_)
    {
        recorder_->buffer().setBounds(static_cast<std::size_t>(capture_max_bytes_),
                                      capture_max_seconds_);
    }

    time_base_->setWindowSeconds(workspace->window_seconds);
    time_base_->setRenderRateHz(workspace->render_rate_hz);

    for (const panel_entry_t& entry : workspace->panels)
    {
        if (entry.type == panel_type_t::unknown)
        {
            // Already reported with its path by validate_workspace. Skipping is
            // the right outcome -- a workspace one panel short beats one that
            // silently grew a panel it does not name.
            continue;
        }
        addPanelFromConfig(entry.config, QString::fromStdString(entry.id));
    }

    // Panels must exist before restoreState(), which matches docks by
    // objectName and drops any it cannot find.
    if (!workspace->dock_state.empty())
    {
        const QByteArray state =
            QByteArray::fromBase64(QByteArray::fromStdString(workspace->dock_state));
        if (!restoreDockState(state))
        {
            // Not an error. The blob is Qt-versioned and opaque; everything that
            // matters is in the YAML, so a failed restore costs an arrangement,
            // not data.
            SPDLOG_WARN("Could not restore the dock arrangement from '{}' -- it was probably "
                        "written by a different Qt version. The panels are all here, arranged "
                        "by default.",
                        path.toStdString());
        }
    }

    workspace_path_ = path;
    workspace_name_ = QString::fromStdString(workspace->name);

    // A freshly loaded workspace is clean, whatever the adds above marked. Same
    // rule as the editor's loadConfigFrom(): the window now matches the file.
    markClean();

    statusBar()->showMessage(tr("Loaded %1").arg(path), 3000);
    return true;
}

// ------------------------------------------------------------------- recordings

bool ScopeWindow::isReviewing() const
{
    return !source_->caps().live;
}

bool ScopeWindow::openRecording(const QString& directory)
{
    auto provider = std::make_unique<BagFileProvider>(directory.toStdString());

    // Checked BEFORE anything is swapped. A failed open that had already
    // replaced the source would drop the window into a review of nothing, which
    // looks exactly like a recording that turned out to be empty.
    if (!provider->isValid())
    {
        SPDLOG_ERROR("'{}' is not a readable bag directory.", directory.toStdString());
        statusBar()->showMessage(
            tr("%1 is not a readable recording. `bag reindex` can rebuild a missing index.")
                .arg(directory),
            8000);
        return false;
    }

    // Reported, not swallowed. A torn part or a non-zero drop count changes how
    // the data should be read: a gap in a trace means something quite different
    // when the recorder is known to have dropped messages, and it is already
    // computed by the time the bag is open.
    const std::vector<std::string> problems = provider->problems();

    const auto [t_begin, t_end] = provider->spanNanos();
    const double duration = t_end > t_begin ? static_cast<double>(t_end - t_begin) / 1e9 : 0.0;

    setSource(std::make_unique<RecordedSource>(std::move(provider)));

    // How many of the workspace's signals this recording does not contain.
    // Surfaced rather than left as empty traces: an unbound signal and a signal
    // that was recorded but never published draw identically, and only one of
    // them is worth chasing.
    std::size_t unbound = 0;
    std::size_t total = 0;
    for (const PanelEntry& entry : panels_)
    {
        if (const auto* plot = qobject_cast<const TimeSeriesPanel*>(entry.panel))
        {
            for (const trace_stats_t& stats : plot->stats().traces)
            {
                ++total;
                if (!stats.bound)
                {
                    ++unbound;
                }
            }
        }
    }

    QString summary = tr("Reviewing %1 (%2 s)").arg(directory).arg(duration, 0, 'f', 1);
    if (unbound > 0)
    {
        summary += tr(" -- %1 of %2 signals are not in this recording")
                       .arg(unbound)
                       .arg(total);
    }
    if (!problems.empty())
    {
        summary += tr(" -- %n problem(s) with the recording", nullptr,
                      static_cast<int>(problems.size()));
        for (const std::string& problem : problems)
        {
            SPDLOG_WARN("{}: {}", directory.toStdString(), problem);
        }
    }

    // The status BAR, not the transport label: that one is owned by
    // updateTransport() and rewritten every frame with the capture's state.
    statusBar()->showMessage(summary, 0);

    SPDLOG_INFO("Reviewing '{}': {:.1f}s, {} problem(s).", directory.toStdString(), duration,
                problems.size());
    return true;
}

bool ScopeWindow::reviewCapture()
{
    if (!recorder_)
    {
        return false;
    }

    const CaptureBuffer& buffer = recorder_->buffer();
    if (buffer.size() == 0)
    {
        SPDLOG_WARN("Nothing has been captured yet.");
        statusBar()->showMessage(
            tr("Nothing captured yet -- no publisher has sent anything since scope started."),
            5000);
        return false;
    }

    // Capture KEEPS RUNNING. Entering review does not stop it: deciding to look
    // at something must not cost you everything that happens while you look.
    // The consequence is that the provider's span moves under the scrubber,
    // which is why updateTransport() re-reads the range every frame and why the
    // retained span and evicted count are on screen.
    setSource(std::make_unique<RecordedSource>(std::make_unique<CaptureProvider>(buffer)));

    statusBar()->showMessage(tr("Reviewing the capture (%1 messages, %2 s retained)")
                                 .arg(buffer.size())
                                 .arg(buffer.retainedSpanSeconds(), 0, 'f', 1),
                             0);
    return true;
}

bool ScopeWindow::saveCaptureTo(const QString& directory)
{
    if (!recorder_ || directory.isEmpty())
    {
        return false;
    }

    if (!recorder_->saveTo(directory.toStdString()))
    {
        SPDLOG_ERROR("Failed to save the capture to '{}'.", directory.toStdString());
        statusBar()->showMessage(tr("Could not save the capture to %1").arg(directory), 8000);
        return false;
    }

    capture_saved_ = true;
    statusBar()->showMessage(tr("Saved the capture to %1").arg(directory), 5000);
    SPDLOG_INFO("Saved {} captured message(s) to '{}'.", recorder_->buffer().size(),
                directory.toStdString());
    return true;
}

bool ScopeWindow::saveCaptureDialog()
{
    if (headless_)
    {
        SPDLOG_WARN("Refusing to raise a Save Recording dialog headlessly. Use "
                    "scope.save_recording with a path.");
        return false;
    }

    // A directory, because a bag is one.
    const QString path = QFileDialog::getExistingDirectory(this, tr("Save Recording"));
    return path.isEmpty() ? false : saveCaptureTo(path);
}

bool ScopeWindow::openRecordingDialog()
{
    if (headless_)
    {
        SPDLOG_WARN("Refusing to raise an Open Recording dialog headlessly. Use "
                    "scope.open_recording with a path.");
        return false;
    }

    // A directory, not a file. A bag is a directory -- metadata.yaml plus the
    // rolled parts -- and a file picker pointed at one .mcap would offer the
    // user a part rather than the recording.
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open Recording"));
    return path.isEmpty() ? false : openRecording(path);
}

void ScopeWindow::goLive()
{
    if (!isReviewing())
    {
        return;
    }

    setSource(std::make_unique<LiveZenohSource>());
    statusBar()->showMessage(tr("Live"), 3000);
}

// ------------------------------------------------------ dialogs and dirty state
//
// Three layers, copied from the editor because the same trap is here: a modal
// dialog raised in a headless run has nobody to dismiss it and hangs the
// process with no diagnostic at all. So the work is dialog-free
// (loadWorkspace/saveWorkspace), the dialogs are thin wrappers that only pick a
// path, and confirmDiscardChanges() answers for itself when there is no one at
// the screen.

bool ScopeWindow::openWorkspaceDialog()
{
    if (headless_)
    {
        SPDLOG_WARN("Refusing to raise an Open dialog headlessly. Use scope.load with a path.");
        return false;
    }

    if (!confirmDiscardChanges(tr("opening another workspace")))
    {
        return false;
    }

    const QString path = QFileDialog::getOpenFileName(this, tr("Open Workspace"), QString(),
                                                      tr("Workspaces (*.yaml *.yml)"));
    return path.isEmpty() ? false : loadWorkspace(path);
}

bool ScopeWindow::saveWorkspaceDialog()
{
    QString path = workspace_path_;
    if (path.isEmpty())
    {
        if (headless_)
        {
            SPDLOG_WARN("No workspace path to save to, and a Save dialog cannot be raised "
                        "headlessly. Use scope.save with a path.");
            return false;
        }
        path = QFileDialog::getSaveFileName(this, tr("Save Workspace"), QString(),
                                            tr("Workspaces (*.yaml *.yml)"));
    }
    return path.isEmpty() ? false : saveWorkspace(path);
}

void ScopeWindow::markDirty()
{
    if (dirty_)
    {
        return;
    }
    dirty_ = true;
    updateWindowTitle();
}

void ScopeWindow::markClean()
{
    if (!dirty_)
    {
        return;
    }
    dirty_ = false;
    updateWindowTitle();
}

void ScopeWindow::updateWindowTitle()
{
    const QString name =
        workspace_name_.isEmpty() ? QStringLiteral("Redline Scope") : workspace_name_;
    setWindowTitle(dirty_ ? name + QStringLiteral(" *") : name);
}

bool ScopeWindow::confirmDiscardChanges(const QString& action)
{
    // An unsaved CAPTURE is the more serious of the two, and is asked about
    // first. A workspace can be rebuilt by hand in a couple of minutes; a
    // capture of what the vehicle was doing cannot be rebuilt at all.
    const bool unsaved_capture =
        recorder_ != nullptr && !capture_saved_ && recorder_->buffer().size() > 0;

    if (!dirty_ && !unsaved_capture)
    {
        return true;
    }

    if (headless_)
    {
        SPDLOG_WARN("Discarding unsaved {}{}{} on {} (headless: nobody to ask).",
                    unsaved_capture ? "capture" : "", unsaved_capture && dirty_ ? " and " : "",
                    dirty_ ? "workspace changes" : "", action.toStdString());
        return true;
    }

    if (unsaved_capture)
    {
        const auto choice = QMessageBox::warning(
            this, tr("Unsaved capture"),
            tr("%1 captured message(s) have not been saved and cannot be recovered "
               "afterwards.\n\nSave the recording before %2?")
                .arg(recorder_->buffer().size())
                .arg(action),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

        if (choice == QMessageBox::Cancel)
        {
            return false;
        }
        if (choice == QMessageBox::Save && !saveCaptureDialog())
        {
            // A failed or cancelled save must not fall through into discarding.
            return false;
        }
    }

    if (!dirty_)
    {
        return true;
    }

    const auto choice = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("This workspace has unsaved changes.\n\nSave before %1?").arg(action),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (choice == QMessageBox::Cancel)
    {
        return false;
    }
    if (choice == QMessageBox::Save)
    {
        saveWorkspaceDialog();
        return !dirty_;
    }
    return true;
}

void ScopeWindow::closeEvent(QCloseEvent* event)
{
    if (!confirmDiscardChanges(tr("closing")))
    {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void ScopeWindow::setHistorySeconds(double seconds)
{
    // Clamped rather than refused: the caller is a workspace file, and declining
    // to load one over a silly number is worse than loading it with a sane one
    // and saying so. The floor is a second because anything less is not history;
    // the ceiling is a day, matching the time base's own window clamp.
    const double clamped = std::clamp(seconds, 1.0, 24.0 * 60.0 * 60.0);
    if (clamped != seconds)
    {
        SPDLOG_WARN("Retention of {}s is outside [1, 86400]; using {}s.", seconds, clamped);
    }
    if (clamped == history_seconds_)
    {
        return;
    }
    history_seconds_ = clamped;

    for (PanelEntry& entry : panels_)
    {
        entry.panel->setHistorySeconds(history_seconds_);
    }

    // The time base clamps a live view to what is retained, so it needs the same
    // number. A copy for the clamp, not a second source of truth -- this is the
    // only place that writes it.
    time_base_->setRetentionSeconds(history_seconds_);

    markDirty();
}

// ----------------------------------------------------------------------- chrome

namespace
{

// How much one press of a zoom button or key changes the span. Wide enough that
// a few presses cross an order of magnitude, narrow enough to land on something
// deliberately.
constexpr double kZoomStep = 1.6;

}  // namespace

void ScopeWindow::buildNavigationActions()
{
    // Actions on the WINDOW, not on a panel, so a key press works wherever the
    // focus happens to be -- the browser, a dock title bar, an empty central
    // area. A panel-level shortcut would work only while a plot had focus,
    // which is a hard thing to notice and an annoying thing to live with.
    const auto add = [this](const char* name, const QString& text, const QKeySequence& key,
                            auto&& on_trigger) {
        auto* action = new QAction(text, this);
        action->setObjectName(name);
        action->setShortcut(key);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, on_trigger);
        addAction(action);
        return action;
    };

    add("action_zoom_in", tr("Zoom &In"), QKeySequence::ZoomIn, [this]() {
        // About the shared cursor when there is one, so zooming in on something
        // you are pointing at keeps it in view; about the middle otherwise.
        const std::optional<double>& cursor = time_base_->cursor();
        time_base_->zoomAt(cursor ? *cursor
                                  : (time_base_->viewBegin() + time_base_->viewEnd()) / 2.0,
                           1.0 / kZoomStep);
    });

    add("action_zoom_out", tr("Zoom &Out"), QKeySequence::ZoomOut, [this]() {
        const std::optional<double>& cursor = time_base_->cursor();
        time_base_->zoomAt(cursor ? *cursor
                                  : (time_base_->viewBegin() + time_base_->viewEnd()) / 2.0,
                           kZoomStep);
    });

    add("action_zoom_fit", tr("&Fit All"), QKeySequence(Qt::CTRL | Qt::Key_0),
        [this]() { time_base_->fitAll(); });

    add("action_pan_back", tr("Pan &Back"), QKeySequence(Qt::Key_Left),
        [this]() { time_base_->panBy(-time_base_->windowSeconds() * 0.1); });

    add("action_pan_forward", tr("Pan &Forward"), QKeySequence(Qt::Key_Right),
        [this]() { time_base_->panBy(time_base_->windowSeconds() * 0.1); });

    // One key for "stop/start whatever is driving the right edge", whichever
    // kind of source is behind the panels. On a live source that is following;
    // on a recording it is playback, which turns following back on itself.
    add("action_toggle_follow", tr("Follow / Play"), QKeySequence(Qt::Key_Space), [this]() {
        if (source_->caps().seekable)
        {
            time_base_->setPlaying(!time_base_->playing());
        }
        else
        {
            time_base_->setFollowing(!time_base_->following());
        }
    });
}

void ScopeWindow::buildMainToolBar()
{
    auto* bar = new QToolBar(tr("Main"), this);

    // Without an objectName QMainWindow::saveState() warns and drops the bar,
    // the same reason transport_bar has one.
    bar->setObjectName("main_toolbar");
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::TopToolBarArea, bar);

    // ------------------------------------------------------------------ mode

    mode_live_ = new QToolButton(bar);
    mode_live_->setObjectName("mode_live");
    mode_live_->setText(tr("● Live"));
    mode_live_->setToolTip(tr("Show what is on the bus now"));
    mode_live_->setCheckable(true);
    connect(mode_live_, &QToolButton::clicked, this, [this]() { goLive(); });
    bar->addWidget(mode_live_);

    mode_review_ = new QToolButton(bar);
    mode_review_->setObjectName("mode_review");
    mode_review_->setText(tr("Review"));
    mode_review_->setToolTip(tr("Review what has been captured since scope started"));
    mode_review_->setCheckable(true);
    mode_review_->setPopupMode(QToolButton::MenuButtonPopup);
    // The button itself reviews the in-memory capture -- "the last live
    // session", which is the case worth one click. Anything else is in the menu.
    connect(mode_review_, &QToolButton::clicked, this, [this]() { (void)reviewCapture(); });

    auto* review_menu = new QMenu(mode_review_);
    review_menu->setObjectName("mode_review_menu");
    // The SAME action the File menu owns, not a copy of it: one objectName, one
    // handler, and the headless guard in openRecordingDialog() already applies.
    if (QAction* open_recording = findChild<QAction*>("action_open_recording"))
    {
        review_menu->addAction(open_recording);
    }
    if (QAction* save_recording = findChild<QAction*>("action_save_recording"))
    {
        review_menu->addAction(save_recording);
    }
    mode_review_->setMenu(review_menu);
    bar->addWidget(mode_review_);

    capture_chip_ = new QLabel(bar);
    capture_chip_->setObjectName("capture_chip");
    capture_chip_->setStyleSheet("color: palette(mid); font-size: 11px; padding: 0 8px;");
    bar->addWidget(capture_chip_);

    // --------------------------------------------------------------- compose

    bar->addSeparator();

    // Generated from the panel table, so a new panel type appears here with no
    // UI code -- the same property the Panels menu has, and from the same list.
    for (const PanelTypeInfo& info : availablePanelTypes())
    {
        const QString type_name =
            QString::fromUtf8(info.name.data(), static_cast<qsizetype>(info.name.size()));
        QAction* action = findChild<QAction*>(QStringLiteral("action_add_%1").arg(type_name));
        if (action == nullptr)
        {
            continue;
        }

        auto* button = new QToolButton(bar);
        // setDefaultAction rather than a new action: the shortcut, the text and
        // the handler all come from the one the menu already has.
        button->setDefaultAction(action);
        button->setText(QStringLiteral("%1 %2").arg(
            QString::fromUtf8(info.toolbar_glyph.data(),
                              static_cast<qsizetype>(info.toolbar_glyph.size())),
            QString::fromUtf8(info.friendly_name.data(),
                              static_cast<qsizetype>(info.friendly_name.size()))));
        button->setToolTip(tr("Add a %1 panel").arg(button->text()));
        bar->addWidget(button);
    }

    // ------------------------------------------------------------------ zoom

    bar->addSeparator();
    const auto add_action_button = [&](const char* action_name, const QString& text) {
        QAction* action = findChild<QAction*>(action_name);
        if (action == nullptr)
        {
            return;
        }
        auto* button = new QToolButton(bar);
        button->setDefaultAction(action);
        button->setText(text);
        bar->addWidget(button);
    };

    add_action_button("action_zoom_out", tr("−"));
    add_action_button("action_zoom_in", tr("+"));
    add_action_button("action_zoom_fit", tr("⤢ Fit"));

    // ------------------------------------------------------- workspace, view

    bar->addSeparator();
    add_action_button("action_open", tr("Open"));
    add_action_button("action_save", tr("Save"));

    bar->addSeparator();
    add_action_button("action_view_browser", tr("Signals"));
}

void ScopeWindow::buildMenuBar()
{
    QMenu* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->setObjectName("menu_file");

    QAction* open = file_menu->addAction(tr("&Open Workspace…"));
    open->setObjectName("action_open");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this]() { (void)openWorkspaceDialog(); });

    QAction* save = file_menu->addAction(tr("&Save Workspace"));
    save->setObjectName("action_save");
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this]() { (void)saveWorkspaceDialog(); });

    file_menu->addSeparator();

    QAction* open_recording = file_menu->addAction(tr("Open &Recording…"));
    open_recording->setObjectName("action_open_recording");
    connect(open_recording, &QAction::triggered, this,
            [this]() { (void)openRecordingDialog(); });

    QAction* review = file_menu->addAction(tr("Stop and Re&view Capture"));
    review->setObjectName("action_review_capture");
    connect(review, &QAction::triggered, this, [this]() { (void)reviewCapture(); });

    QAction* save_recording = file_menu->addAction(tr("Save &Recording…"));
    save_recording->setObjectName("action_save_recording");
    connect(save_recording, &QAction::triggered, this, [this]() { (void)saveCaptureDialog(); });

    QAction* live = file_menu->addAction(tr("Go &Live"));
    live->setObjectName("action_go_live");
    connect(live, &QAction::triggered, this, [this]() { goLive(); });

    // Enabled only when there is something to go back from, so the menu says
    // which mode the window is in without anyone having to read the toolbar.
    live->setEnabled(false);
    connect(this, &ScopeWindow::sourceChanged, live,
            [this, live]() { live->setEnabled(isReviewing()); });

    file_menu->addSeparator();

    QAction* quit = file_menu->addAction(tr("&Quit"));
    quit->setObjectName("action_quit");
    quit->setShortcut(QKeySequence::Quit);
    // close() rather than QApplication::quit(), so the unsaved-changes prompt in
    // closeEvent() is on this path too. Quitting straight out of the menu used
    // to be the one way past it.
    connect(quit, &QAction::triggered, this, [this]() { close(); });

    QMenu* panels_menu = menuBar()->addMenu(tr("&Panels"));
    panels_menu->setObjectName("menu_panels");

    QMenu* add_menu = panels_menu->addMenu(tr("&Add"));
    add_menu->setObjectName("menu_panels_add");

    // Generated from the panel table, so a new panel type appears here with no
    // UI code at all.
    bool first = true;
    for (const PanelTypeInfo& info : availablePanelTypes())
    {
        QAction* action = add_menu->addAction(
            QString::fromUtf8(info.friendly_name.data(),
                              static_cast<qsizetype>(info.friendly_name.size())));
        action->setObjectName(QStringLiteral("action_add_%1")
                                  .arg(QString::fromUtf8(
                                      info.name.data(), static_cast<qsizetype>(info.name.size()))));
        if (first)
        {
            action->setShortcut(QKeySequence::New);
            first = false;
        }
        const panel_type_t type = info.type;
        connect(action, &QAction::triggered, this, [this, type]() { addPanel(type); });
    }

    QMenu* view_menu = menuBar()->addMenu(tr("&View"));
    view_menu->setObjectName("menu_view");
    QAction* show_browser = browser_dock_->toggleViewAction();
    show_browser->setObjectName("action_view_browser");
    show_browser->setText(tr("&Signals"));
    view_menu->addAction(show_browser);
}

namespace
{

// The playback rates the combo offers. Both ends earn their place: 0.1x to
// study a transient you have already found, 20x to find one.
constexpr double kRates[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0};

// How often the overview's histogram may be recomputed. Fast enough that a
// growing capture visibly grows, slow enough that walking millions of retained
// messages under the capture's mutex does not stall the zenoh RX thread that
// needs the same lock to push. See ScopeWindow::refreshDensity().
constexpr std::int64_t kDensityIntervalMs = 500;

QString formatWallClock(std::uint64_t unix_nanos)
{
    if (unix_nanos == 0)
    {
        return {};
    }
    const QDateTime when =
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(unix_nanos / 1'000'000ull));
    return when.toString(QStringLiteral("HH:mm:ss"));
}

}  // namespace

// Both control sets, built once and shown or hidden from caps().
//
// Built once rather than rebuilt per source on purpose: the agent interface
// addresses these by objectName, and widgets recreated on every swap would
// either lose their names or accumulate duplicates of them. Visibility is the
// only thing that changes.
void ScopeWindow::buildTransportBar()
{
    // The overview goes in a bar of its OWN, above the controls, so it gets the
    // full width. Sharing a row with the buttons would leave it a few hundred
    // pixels for a whole recording, which is the resolution the QSlider it
    // replaced had and the reason that slider was useless for finding anything.
    auto* overview_bar = new QToolBar(tr("Overview"), this);
    overview_bar->setObjectName("overview_bar");
    overview_bar->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, overview_bar);

    overview_ = new OverviewStrip(overview_bar);
    overview_bar->addWidget(overview_);
    // Stretches to the bar's width rather than sitting at its natural size,
    // which for a custom widget in a toolbar is the minimum.
    overview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(overview_, &OverviewStrip::viewRequested, this, [this](double begin, double end) {
        if (!updating_transport_)
        {
            time_base_->setView(begin, end);
        }
    });
    connect(overview_, &OverviewStrip::interactionChanged, this,
            [this](bool active) { time_base_->setInteracting(active); });
    connect(overview_, &OverviewStrip::cursorRequested, this,
            [this](std::optional<double> t) { time_base_->setCursor(t); });

    addToolBarBreak(Qt::BottomToolBarArea);

    auto* bar = new QToolBar(tr("Transport"), this);
    bar->setObjectName("transport_bar");
    bar->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, bar);

    // ------------------------------------------------------------------ live

    pause_button_ = new QToolButton(bar);
    pause_button_->setObjectName("transport_pause");
    pause_button_->setCheckable(true);
    pause_button_->setText(tr("Pause"));
    connect(pause_button_, &QToolButton::toggled, this, [this](bool paused) {
        time_base_->setMode(paused ? TimeBase::Mode::Paused : TimeBase::Mode::Live);
        pause_button_->setText(paused ? tr("Live") : tr("Pause"));
    });
    live_controls_.push_back(bar->addWidget(pause_button_));

    // ---------------------------------------------------------------- review

    const auto add_button = [&](const char* name, const QString& text, const QString& tip,
                                bool checkable, auto&& on_click) {
        auto* button = new QToolButton(bar);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(checkable);
        if (checkable)
        {
            connect(button, &QToolButton::toggled, this, on_click);
        }
        else
        {
            connect(button, &QToolButton::clicked, this, on_click);
        }
        review_controls_.push_back(bar->addWidget(button));
        return button;
    };

    add_button("transport_to_start", tr("|◀"), tr("Jump to the start of the recording"), false,
               [this]() { time_base_->seek(time_base_->source().caps().t_begin); });

    add_button("transport_step_back", tr("◀"), tr("Back one second"), false,
               [this]() { time_base_->seek(time_base_->source().now() - 1.0); });

    play_button_ = add_button("transport_play", tr("▶"), tr("Play"), true,
                              [this](bool playing) { time_base_->setPlaying(playing); });

    add_button("transport_step_forward", tr("▶"), tr("Forward one second"), false,
               [this]() { time_base_->seek(time_base_->source().now() + 1.0); });

    add_button("transport_to_end", tr("▶|"), tr("Jump to the end of the recording"), false,
               [this]() { time_base_->seek(time_base_->source().caps().t_end); });

    rate_combo_ = new QComboBox(bar);
    rate_combo_->setObjectName("transport_rate");
    for (const double rate : kRates)
    {
        rate_combo_->addItem(QStringLiteral("%1x").arg(rate), rate);
    }
    rate_combo_->setCurrentIndex(3);  // 1x
    connect(rate_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
        {
            time_base_->setRate(rate_combo_->itemData(index).toDouble());
        }
    });
    review_controls_.push_back(bar->addWidget(rate_combo_));

    // --------------------------------------------------------------- shared

    bar->addSeparator();
    auto* window_label = new QLabel(tr("  Window "), bar);
    bar->addWidget(window_label);

    // Shared, not live-only. What span the plot shows matters at least as much
    // when reviewing a recording as when tailing the bus.
    window_spin_ = new QDoubleSpinBox(bar);
    window_spin_->setObjectName("transport_window_seconds");
    window_spin_->setRange(0.1, 3600.0);
    window_spin_->setDecimals(1);
    window_spin_->setSingleStep(5.0);
    window_spin_->setSuffix(tr(" s"));
    window_spin_->setValue(time_base_->windowSeconds());
    connect(window_spin_, &QDoubleSpinBox::valueChanged, this, [this](double seconds) {
        if (!updating_transport_)
        {
            time_base_->setWindowSeconds(seconds);
        }
    });
    bar->addWidget(window_spin_);

    bar->addSeparator();
    cursor_label_ = new QLabel(bar);
    cursor_label_->setObjectName("transport_cursor");
    cursor_label_->setMinimumWidth(200);
    bar->addWidget(cursor_label_);

    transport_status_ = new QLabel(bar);
    transport_status_->setObjectName("transport_status");
    transport_status_->setStyleSheet("color: palette(mid); font-size: 11px;");
    bar->addWidget(transport_status_);

    connect(time_base_.get(), &TimeBase::cursorMoved, this, [this]() { updateTransport(); });
    connect(time_base_.get(), &TimeBase::changed, this, [this]() { updateTransport(); });

    // The one render timer drives the readout too. A second timer for the
    // transport bar would tick against the panels' and make the position
    // readout disagree with the line beside it.
    connect(time_base_.get(), &TimeBase::frame, this, [this]() { updateTransport(); });

    applySourceCaps();
}

void ScopeWindow::applySourceCaps()
{
    const SourceCaps caps = source_->caps();

    for (QAction* action : live_controls_)
    {
        action->setVisible(caps.live);
    }
    for (QAction* action : review_controls_)
    {
        action->setVisible(caps.seekable);
    }

    // The strip's cached histogram describes the OLD source. Forcing a
    // recompute here rather than waiting for the throttle is what stops a bag's
    // shape being drawn under a live view for half a second after Go Live.
    density_computed_at_ms_ = 0;

    if (play_button_ != nullptr)
    {
        play_button_->setChecked(false);
    }

    // Driven from the SOURCE rather than from the click that changed it, so a
    // swap made by the agent interface, by --bag at startup, or by a review
    // that failed to open leaves the toolbar saying what is actually behind the
    // panels. A pair of buttons that only tracked their own clicks would be
    // wrong in exactly the cases where being right matters.
    const bool reviewing = isReviewing();
    if (mode_live_ != nullptr)
    {
        const bool was_updating = updating_transport_;
        updating_transport_ = true;
        mode_live_->setChecked(!reviewing);
        mode_review_->setChecked(reviewing);
        updating_transport_ = was_updating;
    }

    updateTransport();
}

void ScopeWindow::updateTransport()
{
    if (cursor_label_ == nullptr)
    {
        return;
    }

    const SourceCaps caps = source_->caps();
    const double position = source_->now();

    const bool was_updating = updating_transport_;
    updating_transport_ = true;

    if (window_spin_ != nullptr && window_spin_->value() != time_base_->windowSeconds())
    {
        window_spin_->setValue(time_base_->windowSeconds());
    }

    if (play_button_ != nullptr && play_button_->isChecked() != time_base_->playing())
    {
        // Playback stops itself at the end of the recording, and the button has
        // to follow or it claims to still be playing.
        play_button_->setChecked(time_base_->playing());
    }

    // The cursor if there is one, the playback head otherwise. A recording also
    // has a wall clock, which is the one thing a bag genuinely knows and the
    // live source does not -- so it goes here rather than on the axis, whose
    // relative labels ("-10 s") are what someone reading a trace wants.
    const std::optional<double>& cursor = time_base_->cursor();
    const double t = cursor ? *cursor : position;

    QString text = tr("  t = %1 s").arg(t, 0, 'f', 3);
    if (!caps.live)
    {
        if (const auto* recorded = dynamic_cast<const RecordedSource*>(source_.get()))
        {
            const QString wall = formatWallClock(recorded->wallClockNanosAt(t));
            if (!wall.isEmpty())
            {
                text += QStringLiteral("  (%1)").arg(wall);
            }
        }
    }
    cursor_label_->setText(text);

    // The capture's state, always visible while it is running. A capture whose
    // head is being evicted is the same class of thing as a recorder dropping
    // samples: the part of the session you can still review has a boundary, and
    // it moves. Saying so is what stops someone scrubbing back into a gap and
    // reading it as a publisher that had not started.
    if (recorder_ != nullptr)
    {
        const CaptureBuffer& capture = recorder_->buffer();
        QString state = tr("  ⏺ %1 s captured").arg(capture.retainedSpanSeconds(), 0, 'f', 0);
        if (const std::uint64_t evicted = capture.evicted(); evicted > 0)
        {
            state += tr(", %1 evicted").arg(evicted);
        }
        if (!capture_saved_ && capture.size() > 0)
        {
            state += tr(" (unsaved)");
        }
        if (transport_status_ != nullptr)
        {
            transport_status_->setText(state);
        }
        // Also on the top bar, beside the mode control it argues for: the gap
        // between what is captured and what a live plot can reach is the whole
        // reason to press Review.
        if (capture_chip_ != nullptr)
        {
            capture_chip_->setText(state);
        }
    }

    // A pan or a zoom turns following off without touching the button, so the
    // button has to be read back from the time base rather than left to its own
    // toggled() -- otherwise it sits there saying "Pause" over a frozen plot.
    if (pause_button_ != nullptr && pause_button_->isChecked() == time_base_->following())
    {
        pause_button_->setChecked(!time_base_->following());
        pause_button_->setText(time_base_->following() ? tr("Pause") : tr("Follow"));
    }

    updateOverview();

    updating_transport_ = was_updating;
}

void ScopeWindow::updateOverview()
{
    if (overview_ == nullptr)
    {
        return;
    }

    const SourceCaps caps = source_->caps();
    const double now = source_->now();

    // The extent, and the two source kinds answer it differently for a real
    // reason. A recording has a beginning; a bus does not, so the honest bound
    // for a live source is how far back the panels' buffers still reach.
    double extent_begin = 0.0;
    double extent_end = 0.0;
    if (caps.seekable)
    {
        extent_begin = caps.t_begin;
        extent_end = caps.t_end;
    }
    else
    {
        // Clamped at the source's own epoch: a live source's clock starts at
        // zero and there is nothing before it. Without the clamp a freshly
        // started session shows five minutes of strip for thirty seconds of
        // data, and the histogram -- which can only be counted from the epoch
        // forward -- ends up drawn against a range it was never counted over.
        extent_begin = std::max(now - history_seconds_, 0.0);
        extent_end = now;
    }
    overview_->setExtent(extent_begin, extent_end);

    // What the panels can actually draw. On a recording it is the whole thing;
    // on a live source it is the same as the extent today, and will narrow once
    // the strip can show the capture behind it.
    overview_->setRetained(std::max(extent_begin, now - history_seconds_), extent_end);

    overview_->setView(time_base_->viewBegin(), time_base_->viewEnd());
    overview_->setTimeCursor(time_base_->cursor());

    // Only a seekable source has a position to mark. A live source's "now" is
    // the right edge of the extent, where a playhead would be noise.
    overview_->setPlayhead(caps.seekable ? std::optional<double>(now) : std::nullopt);

    if (recorder_ != nullptr)
    {
        overview_->setEvicted(recorder_->buffer().evicted());
    }

    refreshDensity();
}

bool ScopeWindow::densityFor(double begin, double end, std::size_t buckets,
                             std::vector<std::uint32_t>& out)
{
    if (source_->density(begin, end, buckets, out))
    {
        return true;
    }

    // A live source keeps no history of its own -- only the buffers the panels
    // hold -- so it declines. The recorder has been capturing the whole bus
    // since the window opened, and THAT is the honest picture of where the
    // traffic is. The window is the only thing holding both, which is why the
    // reconciliation lives here rather than behind the DataSource seam.
    const auto* live = dynamic_cast<const LiveZenohSource*>(source_.get());
    if (live == nullptr || recorder_ == nullptr)
    {
        out.clear();
        return false;
    }

    // Seconds on the live source's steady clock -> UNIX nanoseconds, through
    // the wall-clock instant sampled beside its steady epoch. Without that pair
    // the two clocks have no common origin at all.
    const std::uint64_t epoch = live->epochWallNanos();
    const auto to_nanos = [epoch](double t) {
        return epoch + static_cast<std::uint64_t>(std::max(t, 0.0) * 1e9);
    };
    recorder_->buffer().density(to_nanos(begin), to_nanos(end), buckets, out);
    return true;
}

void ScopeWindow::refreshDensity()
{
    if (overview_ == nullptr)
    {
        return;
    }

    // One bucket per pixel of the strip. More would be invisible and cost a
    // longer walk under the capture's mutex; fewer would throw away detail the
    // widget has room to show.
    const int buckets = std::max(overview_->width(), 1);

    const SourceCaps caps = source_->caps();
    const double now = source_->now();

    // Floored at the source's epoch for a LIVE source, and only here: the
    // capture can only be counted from the moment it started, so asking for
    // counts before that would label the histogram with a range it was never
    // counted over. The view itself is deliberately not floored -- see
    // TimeBase::availableRange().
    const double begin = caps.seekable ? caps.t_begin : std::max(now - history_seconds_, 0.0);
    const double end = caps.seekable ? caps.t_end : now;

    const std::int64_t now_ms = QDateTime::currentMSecsSinceEpoch();

    // Two triggers, and each covers what the other cannot.
    //
    // `moved` catches a resize or a source swap, where the cached counts
    // describe a range that is no longer on screen -- those must be redrawn at
    // once, not up to half a second later.
    //
    // `due` covers a capture growing under the strip. The buffer's revision is
    // useless as a cache key here (it bumps on every push, thousands a second),
    // so the clock is what bounds the work. A bag recomputes on this tick too
    // and costs nothing: its answer comes from a handful of part records.
    const bool moved =
        buckets != density_buckets_ || begin != density_begin_ || end != density_end_;
    const bool due = now_ms - density_computed_at_ms_ >= kDensityIntervalMs;

    if (!moved && !due)
    {
        return;
    }

    if (!densityFor(begin, end, static_cast<std::size_t>(buckets), density_))
    {
        // A source that cannot answer cheaply says so, and the strip draws a
        // plain band. Clearing rather than keeping the last answer matters on a
        // swap: a bag's histogram left behind a Go Live would describe a
        // recording that is no longer on screen.
        overview_->setDensity({}, begin, end);
        density_buckets_ = buckets;
        density_begin_ = begin;
        density_end_ = end;
        density_computed_at_ms_ = now_ms;
        return;
    }

    overview_->setDensity(density_, begin, end);
    density_buckets_ = buckets;
    density_begin_ = begin;
    density_end_ = end;
    density_computed_at_ms_ = now_ms;
}

}  // namespace scope

#include "scope/moc_scope_window.cpp"
