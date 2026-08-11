#include "scope/scope_window.h"

#include "scope/add_signal_dialog.h"
#include "scope/data_source.h"
#include "scope/empty_source.h"
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
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

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

    // OFFLINE, and therefore not on the bus at all.
    //
    // A window that opened a zenoh session before anyone asked it to would make
    // "Offline" a label rather than a fact -- and scope is a diagnostic tool, so
    // an instance that quietly joins the bus is exactly the thing you do not
    // want running unattended next to the system you are measuring. Going online
    // is what constructs a LiveZenohSource and a recorder; see goOnline().
    source_ = std::make_unique<EmptySource>();
    time_base_ = std::make_unique<TimeBase>(*source_);

    // The capture's bounds are read now and held, because the recorder they
    // configure does not exist yet: it is built on the way online, possibly
    // several times, and each one needs the limits the workspace last set.
    const scope_workspace_t defaults;
    capture_max_bytes_ = defaults.max_capture_bytes;
    capture_max_seconds_ = defaults.max_capture_seconds;

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

    // AFTER the menu bar, because the landing screen's buttons are the menu's
    // own QActions rather than copies of them -- the same rule the toolbar
    // follows, and for the same reason: one action means one handler and one
    // enabled-state, so a button cannot drift out of step with the menu item
    // that does the same thing.
    buildCentralArea();

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

    // Through the panel's own interface rather than a cast to one kind. This
    // used to be a qobject_cast<TimeSeriesPanel*>, so "Remove signal" was
    // silently missing from every other panel type -- including the video
    // panel, which had a removeStream() nothing ever called.
    const std::vector<QString> bindings = entry->panel->bindingLabels();
    QMenu* remove_menu = nullptr;
    std::vector<QAction*> remove_actions;
    if (!bindings.empty())
    {
        remove_menu = menu.addMenu(tr("Remove signal"));
        remove_menu->setObjectName("menu_panel_remove_signal");
        for (const QString& name : bindings)
        {
            remove_actions.push_back(remove_menu->addAction(name));
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
        if (chosen == remove_actions[i])
        {
            entry->panel->removeBinding(i);
            return;
        }
    }
}

void ScopeWindow::buildCentralArea()
{
    auto* central = new QWidget(this);
    central->setObjectName("central_area");

    // THE CENTRAL AREA MUST NOT COMPETE WITH THE DOCKS FOR WIDTH.
    //
    // QMainWindow honours the central widget's minimum before it gives anything
    // to the docks, and the panels ARE docks -- so a hint wide enough to read
    // squeezes every plot in the window to make room for a sentence. That is
    // exactly what happened when this grew from one short label into a label and
    // two buttons: panels lost ~40 px each and three geometry tests failed on
    // mouse positions that no longer landed where they used to.
    //
    // The DEFAULT policy, deliberately, with the minimum kept small by word wrap
    // on the labels below. Both alternatives were tried and both were wrong:
    //
    //   - QSizePolicy::Ignored reads like the right answer and is the exact
    //     opposite. It means "the sizeHint is ignored, the widget will get as
    //     much space as possible", so it makes the hint GREEDY: panels went from
    //     637 px wide to 160.
    //   - QSizePolicy::Maximum stops it growing at all, so the signal browser
    //     swallowed the whole window whenever there were no panels to hold it
    //     back.
    //
    // What matters is the MINIMUM, not the stretch: word-wrapped labels claim
    // almost no width, so the hint stops competing with the docks while still
    // filling the space nothing else wants.
    central->setMinimumSize(0, 0);

    auto* layout = new QVBoxLayout(central);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(16);

    // ------------------------------------------------------- offline landing

    empty_panel_ = new QWidget(central);
    empty_panel_->setObjectName("offline_hint");
    auto* offline_layout = new QVBoxLayout(empty_panel_);
    offline_layout->setAlignment(Qt::AlignCenter);
    offline_layout->setSpacing(12);
    offline_layout->setContentsMargins(0, 0, 0, 0);

    auto* offline_text = new QLabel(
        tr("<b>Offline</b> — nothing loaded.<br><br>"
           "Load a recording to scrub through it, or go online to watch the live bus."),
        empty_panel_);
    offline_text->setObjectName("offline_hint_text");
    offline_text->setAlignment(Qt::AlignCenter);
    offline_text->setWordWrap(true);
    offline_text->setMinimumWidth(0);
    offline_text->setStyleSheet("color: palette(mid); font-size: 14px;");
    offline_layout->addWidget(offline_text);

    auto* buttons = new QWidget(empty_panel_);
    auto* button_layout = new QHBoxLayout(buttons);
    button_layout->setAlignment(Qt::AlignCenter);
    button_layout->setContentsMargins(0, 0, 0, 0);

    // The MENU'S actions, not copies. Same rule as the toolbar: one action, one
    // handler, one enabled-state -- so the landing screen cannot offer a button
    // the menu has already disabled.
    const auto add_button = [&](const char* action_name, const char* button_name) {
        QAction* action = findChild<QAction*>(action_name);
        if (action == nullptr)
        {
            return;
        }
        auto* button = new QToolButton(buttons);
        button->setObjectName(button_name);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button_layout->addWidget(button);
    };

    add_button("action_open_recording", "offline_load_recording");
    add_button("action_online", "offline_go_online");

    offline_layout->addWidget(buttons);
    layout->addWidget(empty_panel_);

    // ------------------------------------------------------------ no panels

    empty_hint_ =
        new QLabel(tr("No panels yet.\n\nAdd one from Panels ▸ Add, or press Ctrl+N."), central);
    empty_hint_->setObjectName("empty_hint");
    empty_hint_->setAlignment(Qt::AlignCenter);
    empty_hint_->setWordWrap(true);
    empty_hint_->setMinimumWidth(0);
    empty_hint_->setStyleSheet("color: palette(mid); font-size: 14px;");
    layout->addWidget(empty_hint_);

    setCentralWidget(central);
    updateEmptyHint();
}

void ScopeWindow::updateEmptyHint()
{
    // NEITHER hint is shown once there are panels, and that is a layout rule as
    // much as a content one.
    //
    // QMainWindow honours the central widget's minimum before it gives anything
    // to the docks, and the panels ARE docks -- so a hint wide enough to read
    // costs every plot in the window the width it needs. That is not
    // hypothetical: the offline landing screen's two buttons squeezed panels
    // from 637 px to 160 and broke three geometry tests on mouse positions that
    // no longer landed where they used to.
    //
    // Confining both hints to the no-panels case removes the competition
    // entirely rather than trying to win it with size policies -- two of which
    // were tried, and each broke the layout in a different direction. It costs
    // nothing: a window with panels already says what it is on the toolbar, and
    // its central area is a sliver nobody reads.
    const bool nothing_loaded = !isOnline() && !source_->caps().seekable;

    if (empty_panel_ != nullptr)
    {
        // "Offline, nothing loaded" outranks "no panels yet": adding a panel to
        // a window with nothing behind it produces an empty plot, which looks
        // exactly like a signal that is not publishing.
        empty_panel_->setVisible(panels_.empty() && nothing_loaded);
    }
    if (empty_hint_ != nullptr)
    {
        empty_hint_->setVisible(panels_.empty() && !nothing_loaded);
    }

    // THE WHOLE CENTRAL WIDGET, not just its contents.
    //
    // QMainWindowLayout hands the central widget the space left over after the
    // docks, and it does that from the widget's VISIBILITY, not from its size
    // hint: hiding only the labels inside left an empty 840 px container sitting
    // between the browser and the panels, and the panels got 160 px. Emptying
    // the container is not the same as removing it.
    //
    // This is what the old code got for free by making the hint label itself the
    // central widget and calling setVisible() on it.
    if (QWidget* central = centralWidget())
    {
        central->setVisible(panels_.empty());
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

bool ScopeWindow::isOnline() const
{
    return source_->caps().live;
}

bool ScopeWindow::hasCapture() const
{
    return recorder_ != nullptr && recorder_->buffer().size() > 0;
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

    // A bag is an offline source, so opening one takes the window offline --
    // including the capture, which only runs while online. The buffer is kept
    // rather than dropped: Review Session Capture is still how you get back to
    // what the last online session recorded, and a bag opened by mistake should
    // not be able to destroy it.
    if (recorder_ != nullptr)
    {
        recorder_->stop();
    }

    // Set BEFORE the swap, so the chip that applySourceCaps() refreshes at the
    // end of setSource() already describes the recording rather than the source
    // it replaced.
    source_label_ = tr("%1 · %2 s")
                        .arg(QFileInfo(directory).fileName())
                        .arg(duration, 0, 'f', 0);

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
    if (!hasCapture())
    {
        SPDLOG_WARN("Nothing has been captured yet.");
        statusBar()->showMessage(
            tr("Nothing captured yet -- go online first, or load a recording."), 5000);
        return false;
    }

    // The capture stops here, and its buffer does not.
    //
    // ScopeRecorder::stop() drops only the subscriber, which is the difference
    // between a snapshot you can scrub and a dangling pointer: the
    // CaptureProvider below holds a reference into that same buffer for as long
    // as this source lives.
    recorder_->stop();

    const CaptureBuffer& buffer = recorder_->buffer();
    source_label_ = tr("session capture · %1 s").arg(buffer.retainedSpanSeconds(), 0, 'f', 0);

    setSource(std::make_unique<RecordedSource>(std::make_unique<CaptureProvider>(buffer)));

    statusBar()->showMessage(tr("Reviewing the session capture (%1 messages, %2 s)")
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

bool ScopeWindow::goOnline()
{
    if (isOnline())
    {
        return true;
    }

    // Going online starts a NEW capture, so the previous one is about to be
    // gone. Asked BEFORE anything is swapped: a prompt raised halfway through
    // would leave the window online over a capture the user just said they
    // wanted to keep.
    if (!confirmDiscardCapture(tr("starting a new online session")))
    {
        return false;
    }

    source_label_.clear();

    // The source FIRST, then the recorder, and the order is load-bearing. The
    // old source may be a RecordedSource over a CaptureProvider pointing into
    // the recorder's buffer; setSource() destroys it only after every panel has
    // rebound. Replacing the recorder first would free that buffer underneath a
    // source that is still live for a few more statements.
    setSource(std::make_unique<LiveZenohSource>());

    // Everything on the bus, with no exclusions: the point of capturing is that
    // a signal nobody thought to plot can still be added afterwards, and a
    // filter taken from the panels would only ever record what was already on
    // screen -- which is exactly what you do not need after the fact.
    recorder_ = std::make_unique<ScopeRecorder>(static_cast<std::size_t>(capture_max_bytes_),
                                                capture_max_seconds_);
    capture_saved_ = false;

    if (!recorder_->isValid())
    {
        // The window still works, tailing the bus through the live source; it
        // simply has nothing to go back and review. Said out loud because the
        // difference only shows up later, as a Review action that never enables.
        statusBar()->showMessage(
            tr("Online, but the capture could not start -- nothing to review afterwards."), 8000);
    }
    else
    {
        statusBar()->showMessage(tr("Online"), 3000);
    }

    updateModeActions();
    return true;
}

void ScopeWindow::goOffline()
{
    if (!isOnline())
    {
        return;
    }

    // Land on what was just recorded, which is what leaving online is almost
    // always for. reviewCapture() stops the recorder itself; the empty case
    // below has to do it explicitly.
    if (reviewCapture())
    {
        return;
    }

    if (recorder_ != nullptr)
    {
        recorder_->stop();
    }

    source_label_.clear();
    setSource(std::make_unique<EmptySource>());
    statusBar()->showMessage(tr("Offline -- nothing was captured."), 5000);
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

bool ScopeWindow::confirmDiscardCapture(const QString& action)
{
    if (capture_saved_ || !hasCapture())
    {
        return true;
    }

    if (headless_)
    {
        SPDLOG_WARN("Discarding an unsaved capture of {} message(s) on {} (headless: nobody "
                    "to ask).",
                    recorder_->buffer().size(), action.toStdString());
        return true;
    }

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

    // A failed or cancelled save must not fall through into discarding.
    return choice != QMessageBox::Save || saveCaptureDialog();
}

bool ScopeWindow::confirmDiscardChanges(const QString& action)
{
    // An unsaved CAPTURE is the more serious of the two, and is asked about
    // first. A workspace can be rebuilt by hand in a couple of minutes; a
    // capture of what the vehicle was doing cannot be rebuilt at all.
    if (!confirmDiscardCapture(action))
    {
        return false;
    }

    if (!dirty_)
    {
        return true;
    }

    if (headless_)
    {
        SPDLOG_WARN("Discarding unsaved workspace changes on {} (headless: nobody to ask).",
                    action.toStdString());
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

    // ONE button for one bit of state. It reads the state it is IN -- "Offline",
    // "● Online" -- rather than the action it performs, because a button
    // labelled with its action has to be read together with its checked state
    // to know which way round it is, and half the people reading it will get
    // that wrong.
    mode_toggle_ = new QToolButton(bar);
    mode_toggle_->setObjectName("mode_toggle");
    mode_toggle_->setCheckable(true);
    mode_toggle_->setPopupMode(QToolButton::MenuButtonPopup);
    connect(mode_toggle_, &QToolButton::clicked, this, [this](bool online) {
        if (updating_transport_)
        {
            return;
        }
        if (online)
        {
            (void)goOnline();
        }
        else
        {
            goOffline();
        }
        // Whatever happened -- including a transition the user cancelled at the
        // unsaved-capture prompt -- the button is re-checked from the source.
        applySourceCaps();
    });

    auto* mode_menu = new QMenu(mode_toggle_);
    mode_menu->setObjectName("mode_menu");
    // The SAME actions the File menu owns, not copies of them: one objectName,
    // one handler, one enabled-state, and the headless guards inside the dialog
    // wrappers already apply.
    for (const char* name :
         {"action_open_recording", "action_review_capture", "action_save_recording"})
    {
        if (QAction* action = findChild<QAction*>(name))
        {
            mode_menu->addAction(action);
        }
    }
    mode_toggle_->setMenu(mode_menu);
    bar->addWidget(mode_toggle_);

    // Promoted out of that dropdown and onto the bar. With offline as the
    // default, opening a bag is the primary action of a freshly started window,
    // and it used to be two clicks deep behind a button labelled "Review" --
    // a word that does not say "bag" to anyone looking for one.
    if (QAction* open_recording = findChild<QAction*>("action_open_recording"))
    {
        auto* button = new QToolButton(bar);
        button->setObjectName("toolbar_open_recording");
        button->setDefaultAction(open_recording);
        bar->addWidget(button);
    }

    // WHAT is behind the panels, not how the capture is doing -- that is
    // transport_status_ at the other end of the window. This label used to
    // render the same string as that one, from the same line of code.
    source_chip_ = new QLabel(bar);
    source_chip_->setObjectName("source_chip");
    source_chip_->setStyleSheet("color: palette(mid); font-size: 11px; padding: 0 8px;");
    bar->addWidget(source_chip_);

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

    // Checkable rather than a Go Online / Go Offline pair, for the same reason
    // the toolbar has one button: there is one bit of state here, and two
    // actions for one bit is two things that can disagree.
    QAction* online = file_menu->addAction(tr("&Online"));
    online->setObjectName("action_online");
    online->setCheckable(true);
    connect(online, &QAction::triggered, this, [this](bool checked) {
        if (updating_transport_)
        {
            return;
        }
        if (checked)
        {
            (void)goOnline();
        }
        else
        {
            goOffline();
        }
        applySourceCaps();
    });

    file_menu->addSeparator();

    QAction* open_recording = file_menu->addAction(tr("Load &Recording…"));
    open_recording->setObjectName("action_open_recording");
    connect(open_recording, &QAction::triggered, this,
            [this]() { (void)openRecordingDialog(); });

    QAction* review = file_menu->addAction(tr("Re&view Session Capture"));
    review->setObjectName("action_review_capture");
    connect(review, &QAction::triggered, this, [this]() { (void)reviewCapture(); });

    QAction* save_recording = file_menu->addAction(tr("Save &Recording…"));
    save_recording->setObjectName("action_save_recording");
    connect(save_recording, &QAction::triggered, this, [this]() { (void)saveCaptureDialog(); });

    // Both act on a capture, so both are disabled until there is one. They used
    // to be enabled always and answer a click with a status-bar line, which is
    // indistinguishable from a button that is broken -- and on a freshly started
    // window that was every click.
    updateModeActions();

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
    // Sets the mode and NOTHING ELSE. The label is updateTransport()'s, because
    // following can be turned off by a pan or a zoom that never comes through
    // here -- so a handler that also wrote the text would be one of two authors
    // for one label, which is how it ended up with three different words for two
    // states.
    connect(pause_button_, &QToolButton::toggled, this, [this](bool paused) {
        if (!updating_transport_)
        {
            time_base_->setMode(paused ? TimeBase::Mode::Paused : TimeBase::Mode::Live);
        }
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
    // shape being drawn under a live view for half a second after going online.
    density_computed_at_ms_ = 0;

    if (play_button_ != nullptr)
    {
        play_button_->setChecked(false);
    }

    // Driven from the SOURCE rather than from the click that changed it, so a
    // swap made by the agent interface, by --bag at startup, by an open that
    // failed or by a transition the user cancelled leaves the control saying
    // what is actually behind the panels. A button that only tracked its own
    // clicks would be wrong in exactly the cases where being right matters.
    //
    // updating_transport_ guards the writes: setChecked() emits toggled(), and
    // the handlers read the flag and decline, so refreshing the control cannot
    // turn into another mode change.
    const bool online = isOnline();
    {
        const bool was_updating = updating_transport_;
        updating_transport_ = true;

        if (mode_toggle_ != nullptr)
        {
            mode_toggle_->setChecked(online);
            mode_toggle_->setText(online ? tr("● Online") : tr("Offline"));
            mode_toggle_->setToolTip(online
                                         ? tr("Tailing the bus and capturing. Click to go "
                                              "offline and review what was captured.")
                                         : tr("Not on the bus. Click to go online."));
        }
        if (QAction* action = findChild<QAction*>("action_online"))
        {
            action->setChecked(online);
        }

        updating_transport_ = was_updating;
    }

    updateModeActions();
    updateEmptyHint();
    updateTransport();
}

void ScopeWindow::updateModeActions()
{
    const bool has_capture = hasCapture();

    if (QAction* review = findChild<QAction*>("action_review_capture"))
    {
        review->setEnabled(has_capture);
    }
    if (QAction* save = findChild<QAction*>("action_save_recording"))
    {
        save->setEnabled(has_capture);
    }
}

void ScopeWindow::updateSourceChip()
{
    if (source_chip_ == nullptr)
    {
        return;
    }

    if (isOnline())
    {
        const std::uint64_t captured = recorder_ != nullptr ? recorder_->buffer().size() : 0;
        source_chip_->setText(captured > 0
                                  ? tr("⏺ capturing · %1 messages").arg(captured)
                                  : tr("⏺ capturing"));
        return;
    }

    // source_label_ is set by whatever opened the source. Empty means nothing
    // is loaded, which is a state worth naming rather than leaving blank -- a
    // blank chip beside "Offline" reads as a label that failed to render.
    source_chip_->setText(source_label_.isEmpty() ? tr("nothing loaded") : source_label_);
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

    // The capture's state. A capture whose head is being evicted is the same
    // class of thing as a recorder dropping samples: the part of the session you
    // can still review has a boundary, and it moves. Saying so is what stops
    // someone scrubbing back into a gap and reading it as a publisher that had
    // not started.
    //
    // ONE widget shows this. It used to be written to the top bar's chip as
    // well, character for character, from these same lines -- and a string
    // rendered twice in one window is a tell that one of the two has no job of
    // its own. The chip now describes the SOURCE, which is a different question.
    if (transport_status_ != nullptr)
    {
        QString state;
        if (recorder_ != nullptr)
        {
            const CaptureBuffer& capture = recorder_->buffer();
            state = isOnline() ? tr("  ⏺ %1 s captured").arg(capture.retainedSpanSeconds(), 0,
                                                             'f', 0)
                               : tr("  ⏹ %1 s captured").arg(capture.retainedSpanSeconds(), 0,
                                                             'f', 0);
            if (const std::uint64_t evicted = capture.evicted(); evicted > 0)
            {
                state += tr(", %1 evicted").arg(evicted);
            }
            if (!capture_saved_ && capture.size() > 0)
            {
                state += tr(" (unsaved)");
            }
        }
        transport_status_->setText(state);
    }

    updateSourceChip();

    // ONE label pair, driven from ONE place.
    //
    // The button used to be written from here AND from its own toggled()
    // handler, with different words: this one says Pause/Follow, that one said
    // Pause/Live. Which of the three you saw depended on how the state had last
    // been reached, so the same frozen plot could be sitting under a button
    // marked "Live" or one marked "Follow".
    //
    // Reading it back from the time base is the part that has to stay: a pan or
    // a zoom turns following off without touching the button, and a button left
    // to its own toggled() sits there saying "Pause" over a plot that has
    // stopped scrolling.
    if (pause_button_ != nullptr)
    {
        const bool following = time_base_->following();
        pause_button_->setChecked(!following);
        pause_button_->setText(following ? tr("Pause") : tr("Follow"));
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
        // swap: a bag's histogram left behind by going online would describe a
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
