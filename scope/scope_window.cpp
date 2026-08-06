#include "scope/scope_window.h"

#include "scope/add_signal_dialog.h"
#include "scope/data_source.h"
#include "scope/live_zenoh_source.h"
#include "scope/panel.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "time_series/time_series_panel.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <spdlog/spdlog.h>

#include <algorithm>

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

    buildMenuBar();
    buildTransportBar();
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
    std::unique_ptr<Panel> panel = createPanel(config, *source_, nullptr);
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

    panels_.push_back(entry);
    updateEmptyHint();
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
    workspace.name = windowTitle().toStdString();
    workspace.window_seconds = time_base_->windowSeconds();
    workspace.render_rate_hz = static_cast<uint16_t>(time_base_->renderRateHz());

    for (const PanelEntry& entry : panels_)
    {
        panel_entry_t saved;
        saved.id = entry.id.toStdString();
        saved.type = entry.panel->panelType();

        if (const auto* plot = qobject_cast<const TimeSeriesPanel*>(entry.panel))
        {
            saved.config = plot->getConfig();
        }

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
    setWindowTitle(workspace->name.empty() ? QStringLiteral("Redline Scope")
                                           : QString::fromStdString(workspace->name));
    statusBar()->showMessage(tr("Loaded %1").arg(path), 3000);
    return true;
}

// ----------------------------------------------------------------------- chrome

void ScopeWindow::buildMenuBar()
{
    QMenu* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->setObjectName("menu_file");

    QAction* open = file_menu->addAction(tr("&Open Workspace…"));
    open->setObjectName("action_open");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Workspace"), QString(), tr("Workspaces (*.yaml *.yml)"));
        if (!path.isEmpty())
        {
            loadWorkspace(path);
        }
    });

    QAction* save = file_menu->addAction(tr("&Save Workspace"));
    save->setObjectName("action_save");
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this]() {
        QString path = workspace_path_;
        if (path.isEmpty())
        {
            path = QFileDialog::getSaveFileName(this, tr("Save Workspace"), QString(),
                                                tr("Workspaces (*.yaml *.yml)"));
        }
        if (!path.isEmpty())
        {
            saveWorkspace(path);
        }
    });

    file_menu->addSeparator();

    QAction* quit = file_menu->addAction(tr("&Quit"));
    quit->setObjectName("action_quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

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

void ScopeWindow::buildTransportBar()
{
    auto* bar = new QToolBar(tr("Transport"), this);
    bar->setObjectName("transport_bar");
    bar->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, bar);

    pause_button_ = new QToolButton(bar);
    pause_button_->setObjectName("transport_pause");
    pause_button_->setCheckable(true);
    pause_button_->setText(tr("Pause"));
    connect(pause_button_, &QToolButton::toggled, this, [this](bool paused) {
        time_base_->setMode(paused ? TimeBase::Mode::Paused : TimeBase::Mode::Live);
        pause_button_->setText(paused ? tr("Live") : tr("Pause"));
    });
    bar->addWidget(pause_button_);

    bar->addSeparator();
    auto* window_label = new QLabel(tr("  Window "), bar);
    bar->addWidget(window_label);

    window_spin_ = new QDoubleSpinBox(bar);
    window_spin_->setObjectName("transport_window_seconds");
    window_spin_->setRange(0.1, 3600.0);
    window_spin_->setDecimals(1);
    window_spin_->setSingleStep(5.0);
    window_spin_->setSuffix(tr(" s"));
    window_spin_->setValue(time_base_->windowSeconds());
    connect(window_spin_, &QDoubleSpinBox::valueChanged, this,
            [this](double seconds) { time_base_->setWindowSeconds(seconds); });
    bar->addWidget(window_spin_);

    bar->addSeparator();
    cursor_label_ = new QLabel(bar);
    cursor_label_->setObjectName("transport_cursor");
    cursor_label_->setMinimumWidth(160);
    bar->addWidget(cursor_label_);

    connect(time_base_.get(), &TimeBase::cursorMoved, this, [this]() {
        const auto& cursor = time_base_->cursor();
        cursor_label_->setText(cursor ? tr("  t = %1 s").arg(*cursor, 0, 'f', 3)
                                      : QString());
    });

    // A seekable source -- recorded data -- would put a scrubber and a rate
    // control here instead of the Pause button. Reading caps() rather than
    // assuming live is what makes that a change in one place.
    if (!source_->caps().live)
    {
        pause_button_->setEnabled(false);
    }
}

}  // namespace scope

#include "scope/moc_scope_window.cpp"
