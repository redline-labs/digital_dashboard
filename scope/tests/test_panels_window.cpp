// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// ScopeWindow itself: panels and docks, workspaces and dock state,
// source swaps, retention, dirty tracking, and the drop path.

namespace panel_tests
{

// ---------------------------------------------------------------------- window

void testTheWindowAddsAndRemovesPanels()
{
    scope::ScopeWindow window;

    expect(window.panels().empty(), "a new window has no panels");

    const QString first = window.addPanel(scope::panel_type_t::time_series);
    expect(!first.isEmpty(), "adding a panel returns its id");
    expect(window.panels().size() == 1, "the panel is recorded");

    const QString second = window.addPanel(scope::panel_type_t::time_series);
    expect(second != first, "a second panel gets a different id");
    expect(window.panels().size() == 2, "both panels are recorded");

    expect(window.findPanel(first) != nullptr, "a panel can be found by id");
    expect(window.findPanel("nope") == nullptr, "an unknown id finds nothing");

    expect(window.removePanel(first), "removing a panel by id succeeds");
    expect(window.panels().size() == 1, "the panel is gone");
    expect(!window.removePanel(first), "removing it again fails cleanly");
}

void testTheDockTitleFollowsARename()
{
    // The dock's title was set once at creation, so renaming a panel through
    // the Configure dialog or scope.panel_set_config left the old text on the
    // tab -- the one place a panel's name is actually read.
    scope::ScopeWindow window;
    const QString id = window.addPanel(scope::panel_type_t::time_series, "plot");
    scope::ScopeWindow::PanelEntry* entry = window.findPanel(id);
    expect(entry != nullptr, "the panel exists");
    if (entry == nullptr)
    {
        return;
    }
    expect(entry->dock->windowTitle() == entry->panel->title(),
           "the dock starts with the panel's title");

    TimeSeriesPanelConfig_t cfg;
    cfg.title = "Engine";
    expect(scope::applyPanelConfig(*entry->panel, scope::panel_config_variant_t{cfg}),
           "the rename applied");
    expect(entry->dock->windowTitle() == QStringLiteral("Engine"),
           "and the dock's title followed it");
}

void testEveryDockHasAnObjectName()
{
    // Load-bearing. restoreState() silently drops any dock it cannot name, so a
    // workspace would come back missing panels with nothing logged.
    scope::ScopeWindow window;
    const QString id = window.addPanel(scope::panel_type_t::time_series, "my_panel");

    const scope::ScopeWindow::PanelEntry* entry = window.findPanel(id);
    expect(entry != nullptr, "the panel exists");
    if (entry != nullptr)
    {
        expect(entry->dock->objectName() == "my_panel",
               "the dock's objectName is the panel id, which is what restoreState matches on");
    }
}

void testAnExplicitPanelIdIsHonoured()
{
    scope::ScopeWindow window;
    const QString id = window.addPanel(scope::panel_type_t::time_series, "engine");
    expect(id == "engine", "an explicit id is used verbatim, so a workspace can rely on it");
}

void testAddingAnUnknownPanelTypeFails()
{
    scope::ScopeWindow window;
    expect(window.addPanel(scope::panel_type_t::unknown).isEmpty(),
           "an unknown panel type adds nothing and says so");
    expect(window.panels().empty(), "no panel was created");
}

void testDockStateRoundTrips()
{
    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "a");
    window.addPanel(scope::panel_type_t::time_series, "b");

    const QByteArray state = window.dockState();
    expect(!state.isEmpty(), "a window with panels produces a dock state");
    expect(window.restoreDockState(state), "its own dock state restores");
}

void testGarbageDockStateIsRejectedNotFatal()
{
    // The blob is opaque and Qt-versioned, so a workspace written by another Qt
    // build will fail to restore. That has to be survivable: everything that
    // matters is in the readable YAML, and losing this costs an arrangement.
    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "a");

    expect(!window.restoreDockState(QByteArray()), "an empty dock state is refused");
    expect(!window.restoreDockState(QByteArray("not a qt dock state at all")),
           "a corrupt dock state is refused rather than crashing");
    expect(window.panels().size() == 1, "the panels are still there after a failed restore");
}

void testTheTimeBaseIsShared()
{
    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "a");
    window.addPanel(scope::panel_type_t::time_series, "b");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setWindowSeconds(12.5);
    expect(time_base.windowSeconds() == 12.5, "the window length is settable");

    time_base.setMode(scope::TimeBase::Mode::Paused);
    const double frozen = time_base.viewEnd();
    expect(time_base.viewEnd() == frozen, "a paused view end does not move");

    time_base.setMode(scope::TimeBase::Mode::Live);
    expect(time_base.mode() == scope::TimeBase::Mode::Live, "it goes back to live");
}

// --------------------------------------------------------- swapping sources

// A panel moved onto a different source must RELEASE against the old one first.
//
// A handle means nothing to a source that did not issue it, and the window
// destroys the old source only after the panels have rebound -- which is what
// makes the release legal and is the entire ordering rule. Repointing first
// leaves every subscription on the old source alive, and for the live one that
// means zenoh callbacks still decoding samples into buffers nobody will drain.
//
// Mutation-check: move `source_ = &source;` above `releaseAll();` in
// TimeSeriesPanel::rebindTo() and this fails -- the release lands on the new
// source, which has never heard of the handle.
void testRebindingReleasesAgainstTheOldSource()
{
    StubSource first;
    StubSource second;

    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, first);
    plot.addBinding(numericField());

    expect(first.bound.size() == 1, "the signal is bound on the first source");
    expect(second.bound.empty(), "and not on the second");

    const scope::SignalHandle issued = first.bound.empty() ? scope::kInvalidSignal : 1;

    plot.rebindTo(second);

    expect(first.released.size() == 1,
           "the binding was released against the source that ISSUED it (" +
               std::to_string(first.released.size()) + ")");
    expect(!first.released.empty() && first.released[0] == issued,
           "and it is the handle that source handed out");
    expect(second.released.empty(),
           "nothing was released against the new source, which never issued anything");

    expect(second.bound.size() == 1, "and the trace is rebound on the new source");
    if (!second.bound.empty())
    {
        expect(second.bound[0].zenoh_key == "vehicle/engine/rpm",
               "on the same topic -- the workspace is untouched by a source swap");
    }
    expect(plot.getConfig().traces.size() == 1, "the config still describes one trace");

    plot.rebindTo(second);
    expect(second.bound.size() == 1, "rebinding to the source it is already on does nothing");
}

// ------------------------------------------------------------- retention

// `history_seconds` round-trips through the WINDOW, not just through the YAML.
//
// The codec always carried this field and scope_test_workspace always asserted
// it survived a save and a load -- while toWorkspace() never wrote it and
// loadWorkspace() never read it, so retention came from a constant in
// time_series_panel.cpp and the setting did nothing at all. A field that
// round-trips perfectly and is ignored at both ends is the worst kind of dead
// config: every test passes and the knob is not connected to anything.
//
// Mutation-check: drop either the `workspace.history_seconds = ...` in
// toWorkspace() or the setHistorySeconds() call in loadWorkspace(), and one of
// these fails.
void testHistorySecondsReachesThePanels()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "redline_scope_history_test.yaml";
    std::filesystem::remove(path);

    {
        scope::ScopeWindow window;
        window.setHistorySeconds(45.0);
        window.addPanel(scope::panel_type_t::time_series, "plot");

        const scope::ScopeWindow::PanelEntry* entry = window.findPanel("plot");
        expect(entry != nullptr, "the panel exists");
        if (entry != nullptr)
        {
            auto* plot = qobject_cast<scope::TimeSeriesPanel*>(entry->panel);
            expect(plot != nullptr && plot->historySeconds() == 45.0,
                   "a panel is built with the window's retention, not a constant");
        }

        expect(window.toWorkspace().history_seconds == 45.0,
               "and toWorkspace() writes it out");
        expect(window.saveWorkspace(QString::fromStdString(path.string())),
               "the workspace saves");
    }

    {
        scope::ScopeWindow window;
        expect(window.loadWorkspace(QString::fromStdString(path.string())),
               "the workspace loads back");
        expect(window.historySeconds() == 45.0, "loadWorkspace() reads the retention");

        const scope::ScopeWindow::PanelEntry* entry = window.findPanel("plot");
        expect(entry != nullptr, "the panel came back");
        if (entry != nullptr)
        {
            auto* plot = qobject_cast<scope::TimeSeriesPanel*>(entry->panel);
            expect(plot != nullptr && plot->historySeconds() == 45.0,
                   "and its panels are built with it -- retention applies BEFORE binding, "
                   "because a buffer cannot grow a past it never recorded");
        }
    }

    std::filesystem::remove(path);
}

void testRetentionIsClampedNotRefused()
{
    scope::ScopeWindow window;

    window.setHistorySeconds(-1.0);
    expect(window.historySeconds() >= 1.0, "a negative retention is clamped, not accepted");

    window.setHistorySeconds(1e9);
    expect(window.historySeconds() <= 24.0 * 60.0 * 60.0, "an absurd retention is clamped");
}

// ------------------------------------------------------------- dirty state

void testDirtyTracking()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "redline_scope_dirty_test.yaml";
    std::filesystem::remove(path);

    scope::ScopeWindow window;
    expect(!window.isDirty(),
           "an empty window is not dirty -- prompting on the way out of one would train the "
           "user to dismiss the prompt that matters");

    window.addPanel(scope::panel_type_t::time_series, "plot");
    expect(window.isDirty(), "adding a panel makes the workspace dirty");
    expect(window.windowTitle().endsWith(" *"), "and the title says so");

    expect(window.saveWorkspace(QString::fromStdString(path.string())), "it saves");
    expect(!window.isDirty(), "saving makes it clean again");
    expect(!window.windowTitle().endsWith(" *"), "and the marker goes away");

    scope::ScopeWindow::PanelEntry* entry = window.findPanel("plot");
    if (entry != nullptr)
    {
        auto* plot = qobject_cast<scope::TimeSeriesPanel*>(entry->panel);
        expect(plot != nullptr && plot->addBinding(numericField()), "a signal is added");
        expect(window.isDirty(),
               "a panel's own config change reaches the window -- the panel says so rather "
               "than the window guessing from the several routes in");
    }

    window.removePanel("plot");
    expect(window.isDirty(), "removing a panel makes it dirty");

    expect(window.loadWorkspace(QString::fromStdString(path.string())),
           "the saved workspace loads");
    expect(!window.isDirty(), "a freshly loaded workspace is clean");

    // Navigation is looking, not editing. The dirty flag used to be wired to
    // TimeBase::changed, which fires on every pan, zoom and playback tick -- so
    // playing a recording prompted "save changes?" at close for changes the
    // user never made.
    window.timeBase().panBy(-1.0);
    window.timeBase().zoomAt(window.timeBase().viewEnd() - 1.0, 0.5);
    window.timeBase().seek(window.timeBase().viewEnd() - 2.0);
    expect(!window.isDirty(), "panning, zooming and seeking do not dirty the workspace");

    window.timeBase().setWindowSeconds(window.timeBase().windowSeconds() + 5.0);
    expect(window.isDirty(),
           "the explicit window-span setter DOES dirty it -- that value is persisted");

    std::filesystem::remove(path);
}

void testTheConfigDialogEditsARealPanel()
{
    // The reflection-driven dialog is the first GUI route to the reflected
    // configs; before it, right_axis / colours / formats / the map tileset were
    // agent-only. The property worth pinning: a widget edit lands in the PANEL
    // through the same clamped applyPanelConfig path the RPC uses.
    StubSource source;
    std::unique_ptr<scope::Panel> panel =
        scope::createPanel(TimeSeriesPanelConfig_t{}, source);
    expect(panel != nullptr, "dialog: the panel constructed");
    if (panel == nullptr)
    {
        return;
    }

    scope::scope_settings_t settings;
    scope::scope_tileset_t tileset;
    tileset.name = "socal";
    settings.tilesets.push_back(tileset);

    scope::PanelConfigDialog dialog(*panel, settings);

    auto* autoscale = dialog.findChild<QCheckBox*>("config_field_autoscale_y");
    expect(autoscale != nullptr, "dialog: the autoscale editor exists, named by its field");
    auto* window = dialog.findChild<QDoubleSpinBox*>("config_field_window_seconds");
    expect(window != nullptr, "dialog: the window editor exists");
    if (autoscale == nullptr || window == nullptr)
    {
        return;
    }

    autoscale->setChecked(false);
    window->setValue(1e9);  // far beyond the clamp

    auto* buttons = dialog.findChild<QDialogButtonBox*>("config_dialog_buttons");
    expect(buttons != nullptr, "dialog: the button box exists");
    if (buttons == nullptr)
    {
        return;
    }
    buttons->button(QDialogButtonBox::Apply)->click();

    auto* plot = qobject_cast<scope::TimeSeriesPanel*>(panel.get());
    expect(plot != nullptr && !plot->getConfig().autoscale_y,
           "dialog: the checkbox edit reached the panel");
    expect(plot != nullptr && plot->getConfig().window_seconds <= 24.0 * 60.0 * 60.0,
           "dialog: the edit went through the CLAMPED path, same as the RPC");

    // The map panel's tileset renders as a combo fed from settings -- the fix
    // for the panel that could be added from the GUI and never configured.
    std::unique_ptr<scope::Panel> map =
        scope::createPanel(MapPanelConfig_t{}, source);
    expect(map != nullptr, "dialog: the map panel constructed");
    if (map != nullptr)
    {
        scope::PanelConfigDialog map_dialog(*map, settings);
        auto* combo = map_dialog.findChild<QComboBox*>("config_field_tileset");
        expect(combo != nullptr, "dialog: the tileset editor is a combo");
        if (combo != nullptr)
        {
            expect(combo->findText("socal") >= 0,
                   "dialog: it offers the machine's configured tilesets");
            combo->setCurrentText("socal");
            auto* map_buttons =
                map_dialog.findChild<QDialogButtonBox*>("config_dialog_buttons");
            if (map_buttons != nullptr)
            {
                map_buttons->button(QDialogButtonBox::Apply)->click();
            }
            auto* map_panel = qobject_cast<scope::MapPanel*>(map.get());
            expect(map_panel != nullptr && map_panel->getConfig().tileset == "socal",
                   "dialog: the tileset choice reached the map panel");
        }
    }
}

void testOfflineOpensNoZenohSession()
{
    // "Offline opens no zenoh session at all" is a claim about the PROCESS, and
    // it used to be false twice: main() constructed a NodeIdentity at startup
    // (which opens the shared session), and the recorder's TopicDirectory kept
    // a liveliness subscription up for the whole offline session. Everything a
    // window does offline must leave the session manager untouched.
    expect(!pub_sub::SessionManager::isOpen(),
           "no zenoh session is open before any window exists");

    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "plot");
    window.addPanel(scope::panel_type_t::table, "table");

    expect(!pub_sub::SessionManager::isOpen(),
           "an offline window with panels never opened a zenoh session");
}

void testHeadlessNeverBlocksOnADialog()
{
    // THE reason setHeadless() exists. Under --mcp there is nobody to dismiss a
    // modal dialog, so one raised here does not fail -- it hangs the process,
    // with no log line and no error, which is the hardest kind of bug to find
    // from the other side of a socket.
    scope::ScopeWindow window;
    window.setHeadless(true);
    window.addPanel(scope::panel_type_t::time_series, "plot");
    expect(window.isDirty(), "there are unsaved changes to prompt about");

    // Returns rather than hanging: that is the assertion. A test that reached a
    // QMessageBox here would time out instead of failing.
    expect(window.confirmDiscardChanges("closing"),
           "a headless window discards with a warning rather than raising a prompt");

    expect(!window.saveWorkspaceDialog(),
           "and the Save dialog refuses headlessly instead of opening a file picker");
}

// ------------------------------------------------------------------- the drag

void testCandidateEncodingRoundTrips()
{
    const scope::BindingCandidate original = numericField();
    scope::BindingCandidate decoded;

    expect(scope::decodeCandidate(scope::encodeCandidate(original), decoded),
           "an encoded candidate decodes");
    expect(decoded.zenoh_key == original.zenoh_key, "the topic survives the drag");
    expect(decoded.schema_name == original.schema_name, "the schema survives the drag");
    expect(decoded.field_name == original.field_name, "the field survives the drag");
    expect(decoded.type_category == original.type_category, "the category survives the drag");
}

void testGarbageDropDataIsRejectedNotThrown()
{
    // Anything can be dropped on a widget, including a drag from a browser
    // window. This has to fail cleanly: an exception out of a Qt event handler
    // terminates the app, which is exactly how the editor's canvas learned it.
    scope::BindingCandidate decoded;

    expect(!scope::decodeCandidate(QByteArray(), decoded), "empty drop data is refused");
    expect(!scope::decodeCandidate(QByteArray("not json at all"), decoded),
           "non-JSON drop data is refused");
    expect(!scope::decodeCandidate(QByteArray("[1,2,3]"), decoded),
           "JSON that is not an object is refused");
    expect(!scope::decodeCandidate(QByteArray("{}"), decoded),
           "an object with no topic is refused");
    expect(!scope::decodeCandidate(QByteArray("{\"zenoh_key\": 42}"), decoded),
           "an object with a wrong-typed topic is refused");
}

void runWindowTests()
{
    testTheWindowAddsAndRemovesPanels();
    testTheDockTitleFollowsARename();
    testEveryDockHasAnObjectName();
    testAnExplicitPanelIdIsHonoured();
    testAddingAnUnknownPanelTypeFails();
    testDockStateRoundTrips();
    testGarbageDockStateIsRejectedNotFatal();
    testTheTimeBaseIsShared();
    testRebindingReleasesAgainstTheOldSource();
    testHistorySecondsReachesThePanels();
    testRetentionIsClampedNotRefused();
    testDirtyTracking();
    testTheConfigDialogEditsARealPanel();
    testOfflineOpensNoZenohSession();
    testHeadlessNeverBlocksOnADialog();
    testCandidateEncodingRoundTrips();
    testGarbageDropDataIsRejectedNotThrown();
}

}  // namespace panel_tests
