// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// The map panel: role-based drops, pairing diagnostics, and camera
// stats.

namespace panel_tests
{

// ---------------------------------------------------------------- map panel

scope::BindingCandidate positionTopic(
    const std::string& key = "nodes/bd992/gsof/lat_long_height")
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = key;
    candidate.schema_name = "GsofLatLongHeight";
    // An empty field_name is what makes a candidate topic-level.
    return candidate;
}

scope::BindingCandidate numericField(const std::string& key, const std::string& field)
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = key;
    candidate.schema_name = "GsofVelocity";
    candidate.field_name = field;
    candidate.type_category = "float";
    return candidate;
}

// THE DROP THAT JUST WORKS. Dropping a position TOPIC fills latitude and
// longitude together -- the only sensible reading of dropping
// nodes/bd992/gsof/lat_long_height onto a map, and the mirror of the video
// panel's topic-level accept.
void testATopicLevelPositionDropFillsBothCoordinates()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);

    expect(panel.acceptsBinding(positionTopic()), "a recognised position topic is accepted");
    expect(panel.addBinding(positionTopic()), "and the drop lands");

    expect(panel.getConfig().latitude.value_expression == "latitudeDeg",
           "latitude is filled from the schema's own field name");
    expect(panel.getConfig().longitude.value_expression == "longitudeDeg",
           "and so is longitude, from ONE drop");
    expect(panel.getConfig().latitude.zenoh_key == panel.getConfig().longitude.zenoh_key,
           "both on the same topic, which is what lets them pair by timestamp");
    expect(panel.bindingLabels().size() == 2, "and the panel reports two bindings");
}

// A schema this panel has no coordinate table for is DECLINED at the drop
// rather than bound and then silently drawing nothing.
void testAnUnknownTopicIsDeclinedRatherThanBoundAndIgnored()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);

    scope::BindingCandidate candidate;
    candidate.zenoh_key = "vehicle/engine/rpm";
    candidate.schema_name = "EngineRpm";
    expect(!panel.acceptsBinding(candidate),
           "a topic with no latitude/longitude to find is refused");
    expect(!panel.addBinding(candidate), "and cannot be forced in");
}

// Longitude MUST share latitude's topic. Accepting one from elsewhere binds
// cleanly and then draws nothing, with thousands of unpaired samples -- which
// reads as a broken panel rather than as a wrong drop.
void testLongitudeMustShareLatitudesTopic()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);

    scope::BindingCandidate lat;
    lat.zenoh_key = "a/position";
    lat.schema_name = "GsofLatLongHeight";
    lat.field_name = "latitudeDeg";
    lat.type_category = "float";
    expect(panel.addBinding(lat), "the first field drop fills latitude");

    scope::BindingCandidate elsewhere;
    elsewhere.zenoh_key = "b/position";
    elsewhere.schema_name = "GsofLatLongHeight";
    elsewhere.field_name = "longitudeDeg";
    elsewhere.type_category = "float";
    expect(!panel.acceptsBinding(elsewhere), "a longitude from another topic is refused");

    scope::BindingCandidate same = elsewhere;
    same.zenoh_key = "a/position";
    expect(panel.acceptsBinding(same), "one from the same topic is accepted");
    expect(panel.addBinding(same), "and lands");
}

// Roles fill in order, and the label says WHICH -- unlike a plot's traces these
// three are not interchangeable, so "remove the second one" would otherwise be
// a guess.
void testFieldDropsFillLatitudeThenLongitudeThenColour()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);

    panel.addBinding(positionTopic());
    expect(panel.addBinding(numericField("nodes/bd992/gsof/velocity", "horizontalSpeedMps")),
           "a third numeric drop becomes the colour signal");

    const std::vector<QString> labels = panel.bindingLabels();
    expect(labels.size() == 3, "three roles are bound");
    expect(labels[0].startsWith("latitude"), "and the first is named as latitude");
    expect(labels[1].startsWith("longitude"), "the second as longitude");
    expect(labels[2].startsWith("colour"), "and the third as colour");

    expect(panel.removeBinding(2), "the colour binding can be taken back");
    expect(panel.bindingLabels().size() == 2, "leaving the two coordinates");
    expect(panel.getConfig().color_by.zenoh_key.empty(), "and the config cleared with it");
}

// A tileset the settings do not know must SAY SO. "Not configured" and
// "configured but unreadable" are different fixes, and on screen they are the
// same empty panel.
void testAnUnconfiguredTilesetIsNamedInTheDiagnostic()
{
    StubSource source;
    MapPanelConfig_t cfg;
    cfg.tileset = "socal";
    scope::MapPanel panel(cfg, source);
    panel.resize(320, 240);

    // No settings pushed in, so 'socal' resolves to nothing.
    const MapPanelStats_t stats = panel.stats();
    expect(stats.diagnostic.find("socal") != std::string::npos,
           "the caption names the tileset that is missing");
    expect(stats.diagnostic.find("not configured") != std::string::npos,
           "and says it is not configured rather than just empty");
    expect(stats.tiles_requested == 0, "and nothing was asked for");
}

void testAnUnreadableArchiveReportsItsOwnReason()
{
    StubSource source;
    MapPanelConfig_t cfg;
    cfg.tileset = "socal";
    scope::MapPanel panel(cfg, source);

    scope::scope_settings_t settings;
    scope::scope_tileset_t tileset;
    tileset.name = "socal";
    tileset.path = "/definitely/not/here.mbtiles";
    settings.tilesets.push_back(tileset);
    panel.setSettings(settings);

    const MapPanelStats_t stats = panel.stats();
    expect(stats.diagnostic.find("could not be opened") != std::string::npos,
           "an archive that will not open says so");
    expect(stats.diagnostic.find("socal") != std::string::npos, "and names which one");
}

void testAnUnboundPanelSaysSoRatherThanLookingEmpty()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);

    const MapPanelStats_t stats = panel.stats();
    expect(stats.diagnostic.find("No position bound") != std::string::npos,
           "a panel with nothing bound says what to do about it");
    expect(!stats.latitude_bound && !stats.longitude_bound, "and reports both roles unbound");
}

// The marker instant is the SHARED one -- the cursor when there is one,
// otherwise the view's right edge. A marker drawn for any other instant looks
// exactly like a correct one, which is why this is asserted on numbers.
void testTheMarkerReadsTheSharedInstant()
{
    StubSource source;
    scope::TimeBase time_base(source);

    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);
    panel.setTimeBase(&time_base);
    panel.resize(320, 240);

    expect(!panel.stats().at_cursor, "with no cursor the panel reads the view's right edge");

    time_base.setCursor(42.0);
    const MapPanelStats_t stats = panel.stats();
    expect(stats.at_cursor, "with a cursor set, the panel reads THAT");
}

void testThePanelReportsItsCameraAfterAPaint()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);
    panel.resize(200, 150);
    panel.repaint();

    const MapPanelStats_t stats = panel.stats();
    expect(stats.camera_zoom > 0.0, "the camera is reported after a paint");
    expect(!stats.camera_moved, "and nothing has panned it");
}

// The compass and view buttons: session-only overrides that never touch the
// workspace config, and a recentre that restores Follow Cursor.
void testTheCameraModesAreSessionOverrides()
{
    StubSource source;
    MapPanelConfig_t cfg;
    cfg.pitch = 50.0;
    cfg.bearing = 30.0;
    scope::MapPanel panel(cfg, source);
    panel.resize(300, 200);
    panel.show();
    panel.repaint();

    auto* view = panel.findChild<QAbstractButton*>(QStringLiteral("mapViewModeButton"));
    auto* compass = panel.findChild<QAbstractButton*>(QStringLiteral("mapCompassButton"));
    expect(view != nullptr && compass != nullptr,
           "an interactive panel carries the view and compass buttons");
    if (view == nullptr || compass == nullptr)
    {
        return;
    }

    expect(std::abs(panel.stats().camera_pitch) < 1e-12, "the panel opens flat");
    view->click();
    panel.repaint();
    expect(std::abs(panel.stats().camera_pitch - 50.0) < 1e-12,
           "one press tilts to the configured pitch");
    expect(panel.getConfig().view_mode == MapViewMode_t::top_down,
           "without writing the mode into the workspace config");

    // With no track there is no course, so course_up falls back to the
    // configured bearing rather than snapping to zero.
    compass->click();
    panel.repaint();
    expect(std::abs(panel.stats().camera_bearing - cfg.bearing) < 1e-12,
           "course-up with no track keeps the configured bearing");
}

// The way back the panel never had: recentre clears both the pan and the
// wheel zoom, and the button only exists while there is something to undo.
void testRecentreRestoresFollowCursor()
{
    StubSource source;
    MapPanelConfig_t cfg;
    scope::MapPanel panel(cfg, source);
    panel.resize(300, 200);
    panel.show();
    panel.repaint();

    auto* recentre = panel.findChild<QAbstractButton*>(QStringLiteral("mapRecentreButton"));
    expect(recentre != nullptr, "the recentre button exists");
    if (recentre == nullptr)
    {
        return;
    }
    expect(recentre->isHidden(), "and hides while there is nothing to undo");

    // A drag: press away from any track (there is none), move, release.
    // Synthesised events rather than QTest, which the scope tests do not link.
    const auto mouse = [&](QEvent::Type type, QPointF at) {
        QMouseEvent event(type, at, panel.mapToGlobal(at),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&panel, &event);
    };
    mouse(QEvent::MouseButtonPress, QPointF(150, 100));
    mouse(QEvent::MouseMove, QPointF(180, 130));
    mouse(QEvent::MouseButtonRelease, QPointF(180, 130));
    panel.repaint();
    expect(panel.stats().camera_moved, "a drag suspends Follow Cursor");
    // isHidden rather than isVisible: under the offscreen platform the panel
    // window is never mapped, and isVisible would ask about the whole chain.
    expect(!recentre->isHidden(), "and the recentre button appears");

    recentre->click();
    panel.repaint();
    expect(!panel.stats().camera_moved, "recentre restores Follow Cursor");
    expect(recentre->isHidden(), "and the button hides again");
}

// The zoom pair steps the panel's camera like the wheel does -- including
// breaking Follow Cursor, which recentre then undoes together with the pan.
void testTheZoomButtonsStepThePanelCamera()
{
    StubSource source;
    MapPanelConfig_t cfg;
    cfg.zoom = 12.0;
    scope::MapPanel panel(cfg, source);
    panel.resize(300, 260);
    panel.show();
    panel.repaint();

    auto* zoomIn = panel.findChild<QAbstractButton*>(QStringLiteral("mapZoomInButton"));
    auto* recentre = panel.findChild<QAbstractButton*>(QStringLiteral("mapRecentreButton"));
    expect(zoomIn != nullptr && recentre != nullptr, "the panel has the zoom pair");
    if (zoomIn == nullptr || recentre == nullptr)
    {
        return;
    }

    zoomIn->click();
    expect(std::abs(panel.stats().camera_zoom - 13.0) < 1e-9,
           "one press of plus zooms a level");
    expect(!recentre->isHidden(),
           "and, wheel-like, it suspends Follow Cursor so recentre appears");

    recentre->click();
    expect(std::abs(panel.stats().camera_zoom - 12.0) < 1e-9,
           "recentre restores the configured zoom");
}

// Compass drag and straighten-first click, panel edition.
void testTheCompassSpinAndStraightenOnThePanel()
{
    StubSource source;
    MapPanelConfig_t cfg;
    cfg.bearing = 0.0;
    scope::MapPanel panel(cfg, source);
    panel.resize(300, 260);
    panel.show();
    panel.repaint();

    panel.setManualBearing(120.0);
    panel.repaint();
    expect(std::abs(panel.stats().camera_bearing - 120.0) < 1e-9,
           "a needle drag spins the panel's map");

    panel.cycleOrientation();
    panel.repaint();
    expect(std::abs(panel.stats().camera_bearing) < 1e-9,
           "the first compass click straightens");
    expect(panel.effectiveOrientation() == MapPanelOrientation_t::north_up,
           "without changing the mode");

    panel.cycleOrientation();
    expect(panel.effectiveOrientation() == MapPanelOrientation_t::course_up,
           "and the next click cycles to course-up");
}

void runMapPanelTests()
{
    testATopicLevelPositionDropFillsBothCoordinates();
    testAnUnknownTopicIsDeclinedRatherThanBoundAndIgnored();
    testLongitudeMustShareLatitudesTopic();
    testFieldDropsFillLatitudeThenLongitudeThenColour();
    testAnUnconfiguredTilesetIsNamedInTheDiagnostic();
    testAnUnreadableArchiveReportsItsOwnReason();
    testAnUnboundPanelSaysSoRatherThanLookingEmpty();
    testTheMarkerReadsTheSharedInstant();
    testThePanelReportsItsCameraAfterAPaint();
    testTheCameraModesAreSessionOverrides();
    testRecentreRestoresFollowCursor();
    testTheZoomButtonsStepThePanelCamera();
    testTheCompassSpinAndStraightenOnThePanel();
}

}  // namespace panel_tests
