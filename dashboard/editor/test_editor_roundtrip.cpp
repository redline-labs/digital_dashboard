// The editor's own load -> edit -> export path, driven through the real Canvas
// and PropertiesPanel rather than the YAML codec underneath them.
//
// The codec test next door pins the file format. This one pins the layer above
// it, which is where the actual data loss lived: export hardcoded the window
// name, and the properties panel cached one form per widget *class*, so two
// widgets of the same type shared a page and Apply wrote the wrong one's values
// into the other.
//
// Labelled gui: it constructs Qt widgets, on the offscreen platform.

#include "editor/canvas.h"
#include "editor/properties_panel.h"
#include "editor/selection_frame.h"

#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// The document as it would be written to disk. Not named `emit`: Qt defines
// that as a macro.
std::string toYaml(const app_config_t& cfg)
{
    YAML::Emitter out;
    out << YAML::convert<app_config_t>::encode(cfg);
    return out.c_str();
}

app_config_t twoStaticTexts(const std::string& window_name)
{
    app_config_t cfg;
    cfg.name = window_name;
    cfg.width = 640;
    cfg.height = 480;
    cfg.background_color = helpers::Color("#101010");

    for (const char* text : {"first", "second"})
    {
        widget_config_t wc;
        wc.type = StaticTextWidget::kWidgetType;
        wc.id = text;
        wc.x = 10;
        wc.y = 20;
        wc.width = 100;
        wc.height = 30;
        StaticTextConfig_t sc;
        sc.text = text;
        wc.config = sc;
        cfg.widgets.push_back(wc);
    }
    return cfg;
}

// Export replaced the window name with a hardcoded "editor_window", so loading
// a config and saving it renamed it. The codec test cannot see this -- the loss
// happens above the codec.
void testWindowIdentitySurvivesTheEditor()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("instrument_cluster"));

    const app_config_t out = canvas.exportAppConfig();
    check(out.name == "instrument_cluster",
          "the window name survives load -> export, got '" + out.name + "'");
    check(out.width == 640 && out.height == 480, "the window size survives load -> export");
    check(out.widgets.size() == 2, "both widgets survive load -> export");
}

void testWidgetIdentityAndConfigSurviveTheEditor()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));

    const app_config_t out = canvas.exportAppConfig();
    if (out.widgets.size() != 2)
    {
        check(false, "expected two widgets back");
        return;
    }

    check(out.widgets[0].id == "first" && out.widgets[1].id == "second",
          "ids survive load -> export, and stay in order");
    check(std::get<StaticTextConfig_t>(out.widgets[0].config).text == "first" &&
              std::get<StaticTextConfig_t>(out.widgets[1].config).text == "second",
          "each widget keeps its own text through load -> export");
}

// The page cache was keyed by Qt class name, so selecting the second static_text
// showed the first one's form. Two widgets of the same type is the whole point
// of the case.
void testSelectingASecondWidgetOfTheSameTypeShowsItsOwnValues()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);
    QObject::connect(&canvas, &Canvas::selectionChanged, &panel, &PropertiesPanel::setSelectedWidget);

    canvas.loadFromAppConfig(twoStaticTexts("w"));
    const auto frames = canvas.frames();
    if (frames.size() != 2)
    {
        check(false, "expected two frames on the canvas");
        return;
    }

    // The form names its editors "field:<path>", so the text field is findable
    // without knowing how the form is laid out.
    const auto shownText = [&panel]() -> std::string
    {
        QLineEdit* edit = panel.findChild<QLineEdit*>("field:text");
        return edit ? edit->text().toStdString() : "<no text field>";
    };

    canvas.selectFrame(frames[0]);
    check(shownText() == "first", "selecting the first widget shows its text, got '" + shownText() + "'");

    canvas.selectFrame(frames[1]);
    check(shownText() == "second",
          "selecting the second widget of the same type shows ITS text, got '" + shownText() + "'");

    canvas.selectFrame(frames[0]);
    check(shownText() == "first", "selecting back shows the first widget's text again");
}

// Deleting the selected widget used to be a second, drifted copy of the
// selection logic. Exercise the paths together.
void testDeletingTheSelectionLeavesAConsistentCanvas()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));

    auto frames = canvas.frames();
    if (frames.size() != 2)
    {
        check(false, "expected two frames on the canvas");
        return;
    }

    canvas.selectFrame(frames[0]);
    check(canvas.removeFrame(frames[0]), "the selected widget can be removed");

    const app_config_t out = canvas.exportAppConfig();
    check(out.widgets.size() == 1, "export reflects the deletion");
    check(out.widgets.size() == 1 && out.widgets[0].id == "second",
          "the surviving widget is the one that was not deleted");

    // Selecting and exporting again must not trip over the deleted frame.
    frames = canvas.frames();
    check(frames.size() == 1, "the canvas lists one frame after the deletion");
    if (!frames.empty())
    {
        canvas.selectFrame(frames[0]);
        check(canvas.exportAppConfig().widgets.size() == 1, "export is stable after re-selecting");
    }
}

// Derived names used to come from items_.size(), so deleting one widget and
// adding another handed out a name that was already in use. An agent selector
// matching two widgets is an AMBIGUOUS_SELECTOR error, not a coin toss.
void testDerivedNamesAreNotReusedAfterADelete()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));

    SelectionFrame* a = canvas.addWidget(widget_type_t::static_text, QPoint(0, 0));
    SelectionFrame* b = canvas.addWidget(widget_type_t::static_text, QPoint(10, 10));
    if (!a || !b)
    {
        check(false, "expected two widgets to be added");
        return;
    }

    const QString name_b = b->objectName();
    check(a->objectName() != name_b, "two added widgets get different names");

    check(canvas.removeFrame(a), "the first added widget is removed");

    SelectionFrame* c = canvas.addWidget(widget_type_t::static_text, QPoint(20, 20));
    check(c != nullptr && c->objectName() != name_b,
          "a widget added after a delete does not reuse a live name (got '" +
              (c ? c->objectName().toStdString() : std::string("<null>")) + "', live is '" +
              name_b.toStdString() + "')");

    // ...and it must not collide with the names the loaded config produced.
    for (SelectionFrame* frame : canvas.frames())
    {
        if (frame != c)
        {
            check(frame->objectName() != c->objectName(),
                  "the new name does not collide with '" + frame->objectName().toStdString() + "'");
        }
    }
}

// The naming index counts config entries, not created frames. A config with one
// unrecognised widget in it would otherwise shift every later widget's derived
// name in the editor relative to the dashboard.
void testAnUnknownWidgetDoesNotShiftLaterNames()
{
    app_config_t cfg = twoStaticTexts("w");

    // Insert an entry the editor will skip, ahead of the others.
    widget_config_t broken;
    broken.type = widget_type_t::unknown;
    cfg.widgets.insert(cfg.widgets.begin(), broken);

    // Strip the explicit ids so the derived names are what gets tested.
    for (auto& wc : cfg.widgets)
    {
        wc.id.clear();
    }

    Canvas canvas;
    canvas.loadFromAppConfig(cfg);

    const auto frames = canvas.frames();
    check(frames.size() == 2, "the unknown widget is skipped, the others are not");
    if (frames.size() == 2)
    {
        // Config indices 1 and 2 -- what MainWindow would derive for the same file.
        check(frames[0]->objectName() == "static_text#1",
              "the first real widget keeps its config index, got '" +
                  frames[0]->objectName().toStdString() + "'");
        check(frames[1]->objectName() == "static_text#2",
              "the second real widget keeps its config index, got '" +
                  frames[1]->objectName().toStdString() + "'");
    }
}

// ------------------------------------------------------------- edit history

std::vector<std::string> idsOf(const Canvas& canvas)
{
    std::vector<std::string> out;
    for (const SelectionFrame* frame : canvas.frames())
    {
        out.push_back(frame->objectName().toStdString());
    }
    return out;
}

void testUndoAndRedoWalkTheEditHistory()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();
    canvas.markSaved();

    check(!canvas.canUndo() && !canvas.canRedo(), "a freshly loaded canvas has no history");
    check(!canvas.undo(), "undo on an empty history reports that it did nothing");

    const auto after_load = idsOf(canvas);

    canvas.addWidget(widget_type_t::static_text, QPoint(0, 0));
    canvas.addWidget(widget_type_t::value_readout, QPoint(10, 10));
    const auto after_two_adds = idsOf(canvas);
    check(after_two_adds.size() == after_load.size() + 2, "both widgets were added");

    check(canvas.undo(), "the second add is undone");
    check(idsOf(canvas).size() == after_load.size() + 1, "one widget is left");
    check(canvas.undo(), "the first add is undone");
    check(idsOf(canvas) == after_load, "the canvas is back to the loaded state");
    check(!canvas.canUndo(), "the history is exhausted");

    check(canvas.redo(), "the first add is redone");
    check(canvas.redo(), "the second add is redone");
    check(idsOf(canvas) == after_two_adds,
          "redoing both restores exactly what was there, names included");
}

// Undo restores through loadFromAppConfig, which derives names from config
// position -- so without care a widget created as static_text#4 comes back as
// static_text#3, silently invalidating any selector held on it.
void testNamesSurviveAnUndo()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    SelectionFrame* added = canvas.addWidget(widget_type_t::static_text, QPoint(0, 0));
    if (!added)
    {
        check(false, "expected a widget to be added");
        return;
    }
    const std::string name = added->objectName().toStdString();

    canvas.addWidget(widget_type_t::value_readout, QPoint(10, 10));
    check(canvas.undo(), "the second add is undone");

    const auto ids = idsOf(canvas);
    check(std::find(ids.begin(), ids.end(), name) != ids.end(),
          "the surviving widget keeps the name it was created with ('" + name + "')");
}

// A no-op edit must not land on the stack: an undo step that appears to do
// nothing is worse than no undo step.
void testNoOpEditsAreNotRecorded()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    canvas.beginEdit();
    canvas.commitEdit();
    check(!canvas.canUndo(), "an edit that changed nothing is not recorded");

    // A drag that ends where it started is the real-world version of this.
    const auto frames = canvas.frames();
    if (!frames.empty())
    {
        canvas.beginEdit();
        const QPoint origin = frames[0]->pos();
        frames[0]->move(origin + QPoint(50, 50));
        frames[0]->move(origin);
        canvas.commitEdit();
        check(!canvas.canUndo(), "a move that ends where it started is not recorded");
    }
}

void testDirtyTracksTheLastSave()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();
    canvas.markSaved();

    check(!canvas.isDirty(), "a just-saved canvas is clean");

    canvas.addWidget(widget_type_t::static_text, QPoint(0, 0));
    check(canvas.isDirty(), "adding a widget makes it dirty");

    // Undoing back to the saved state makes it clean again -- comparing against
    // the saved snapshot rather than counting edits is what buys this.
    check(canvas.undo(), "the add is undone");
    check(!canvas.isDirty(), "undoing back to the saved state is clean again");

    canvas.addWidget(widget_type_t::value_readout, QPoint(5, 5));
    check(canvas.isDirty(), "a further edit is dirty");
    canvas.markSaved();
    check(!canvas.isDirty(), "saving makes the current state the new baseline");
}

// ------------------------------------------------------- window properties
//
// The window's name, size and background were the one mutation path that never
// opened a history entry. They changed the canvas, set the dirty flag (isDirty
// compares snapshots, so it noticed) and could not be undone -- and because the
// *next* edit's beginEdit() snapshotted the already-changed state, they were
// baked in permanently. Nothing emitted historyChanged either, so the title bar
// kept its old text until an unrelated edit refreshed it.

// What typing into a QLineEdit and then leaving it produces. setText() alone is
// silent on textEdited: Qt reserves that signal for user input.
void typeInto(QLineEdit* edit, const QString& text)
{
    edit->setText(text);
    emit edit->textEdited(text);
    emit edit->editingFinished();
}

void testWindowPropertiesAreUndoable()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);

    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();
    canvas.markSaved();

    auto* bg = panel.findChild<QLineEdit*>("window:background_color");
    if (bg == nullptr)
    {
        check(false, "the window page has an addressable background field");
        return;
    }

    check(canvas.getBackgroundColorHex() == "#101010", "the loaded background is in place");

    typeInto(bg, "#ff0000");
    check(canvas.getBackgroundColorHex() == "#ff0000", "editing the field changes the background");
    check(canvas.isDirty(), "a window edit makes the document dirty");
    check(canvas.canUndo(), "a window edit is undoable");

    check(canvas.undo(), "the window edit is undone");
    check(canvas.getBackgroundColorHex() == "#101010",
          "undo restores the background, got '" + canvas.getBackgroundColorHex().toStdString() + "'");
    check(!canvas.isDirty(), "undoing back to the saved state is clean again");
}

void testWindowNameIsUndoable()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);

    canvas.loadFromAppConfig(twoStaticTexts("before"));
    canvas.clearHistory();

    auto* name = panel.findChild<QLineEdit*>("window:name");
    if (name == nullptr)
    {
        check(false, "the window page has an addressable name field");
        return;
    }

    typeInto(name, "after");
    check(canvas.windowName() == "after", "editing the field renames the window");
    check(canvas.undo(), "the rename is undoable");
    check(canvas.windowName() == "before",
          "undo restores the window name, got '" + canvas.windowName() + "'");
}

// One entry per edited field, not one per character. beginEdit() collapses the
// keystrokes; editingFinished closes the entry.
void testTypingAWindowFieldIsOneHistoryEntry()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);

    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    auto* bg = panel.findChild<QLineEdit*>("window:background_color");
    if (bg == nullptr)
    {
        check(false, "the window page has an addressable background field");
        return;
    }

    // Every prefix a real typist produces, including the half-formed ones.
    for (const char* prefix : {"#", "#a", "#aa", "#aab", "#aabb", "#aabbc", "#aabbcc"})
    {
        bg->setText(prefix);
        emit bg->textEdited(prefix);
    }
    emit bg->editingFinished();

    check(canvas.getBackgroundColorHex() == "#aabbcc", "the finished value is applied");
    check(canvas.undo(), "the typing is undoable");
    check(canvas.getBackgroundColorHex() == "#101010",
          "a single undo goes back past the whole word, not one character, got '" +
              canvas.getBackgroundColorHex().toStdString() + "'");
    check(!canvas.canUndo(), "typing one field left exactly one history entry");
}

// An open window edit must not swallow the next unrelated one.
//
// The window fields close their entry on editingFinished, which needs a focus
// change -- and the agent control interface never touches focus. So a colour
// typed over the control socket stayed open, and the next editor.move joined it:
// two edits, one undo step, and undoing the move silently reverted the colour
// too. Caught by driving the real sequence through the agent path.
void testAnOpenWindowEditDoesNotSwallowTheNextEdit()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);

    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    auto* bg = panel.findChild<QLineEdit*>("window:background_color");
    const auto frames = canvas.frames();
    if (bg == nullptr || frames.empty())
    {
        check(false, "expected a background field and at least one frame");
        return;
    }

    // Typed, but never finished -- no Enter, no focus change.
    bg->setText("#00ff00");
    emit bg->textEdited("#00ff00");
    check(canvas.getBackgroundColorHex() == "#00ff00", "the colour is applied while still open");

    // Now an unrelated edit, as editor.move would make it.
    const QString moved_name = frames[0]->objectName();
    const QPoint origin = frames[0]->pos();
    canvas.beginEdit();
    frames[0]->move(origin + QPoint(40, 40));
    canvas.commitEdit();

    check(canvas.undo(), "the move is undone");

    // Re-resolve by name rather than reusing the pointer: restore() rebuilds
    // every widget, so anything held across an undo is a dead object.
    const auto after = canvas.frames();
    const auto it = std::find_if(after.begin(), after.end(),
                                 [&](SelectionFrame* f) { return f->objectName() == moved_name; });
    check(it != after.end() && (*it)->pos() == origin, "undo puts the widget back");
    check(canvas.getBackgroundColorHex() == "#00ff00",
          "undoing the move leaves the colour alone, got '" +
              canvas.getBackgroundColorHex().toStdString() + "'");

    check(canvas.undo(), "the colour is a separate entry, still undoable");
    check(canvas.getBackgroundColorHex() == "#101010",
          "the second undo reverts the colour, got '" +
              canvas.getBackgroundColorHex().toStdString() + "'");
}

// Half-typed text arrives on every keystroke, and an unparseable value silently
// becomes the fallback colour -- so applying it made the preview flicker through
// black on the way to the real value.
//
// Note "#123" is a colour, not a prefix: every six-digit value passes through a
// legitimate three-digit one while being typed, and applying that is correct.
// What must never happen is a *malformed* prefix reaching the canvas.
void testHalfTypedColoursAreNotApplied()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);

    canvas.loadFromAppConfig(twoStaticTexts("w"));

    auto* bg = panel.findChild<QLineEdit*>("window:background_color");
    if (bg == nullptr)
    {
        check(false, "the window page has an addressable background field");
        return;
    }

    // Typing "#123456", one character at a time, with what the canvas should be
    // showing after each. It only moves on the two prefixes that are colours.
    const std::vector<std::pair<const char*, const char*>> typing = {
        {"#", "#101010"},      {"#1", "#101010"},      {"#12", "#101010"},
        {"#123", "#123"},      {"#1234", "#123"},      {"#12345", "#123"},
        {"#123456", "#123456"},
    };

    for (const auto& [typed, expected] : typing)
    {
        bg->setText(typed);
        emit bg->textEdited(typed);
        check(canvas.getBackgroundColorHex() == QString(expected),
              std::string("after typing '") + typed + "' the background is '" + expected +
                  "', got '" + canvas.getBackgroundColorHex().toStdString() + "'");
    }
}

// The background used to live only in the Canvas's QPalette, read back out with
// QColor::name(). That is lossy twice over: Qt reads "#RRGGBBAA" as "#AARRGGBB",
// and name() drops alpha. "#112233ff" came back "#2233ff" and was saved over the
// original.
void testBackgroundSurvivesTheCanvas()
{
    Canvas canvas;
    for (const char* colour : {"#112233ff", "#abc", "#AABBCC", "#11223300"})
    {
        canvas.setBackgroundColor(colour);
        check(canvas.getBackgroundColorHex() == QString(colour),
              std::string(colour) + " survives the canvas verbatim, got '" +
                  canvas.getBackgroundColorHex().toStdString() + "'");
    }

    // And through a whole load -> export cycle.
    app_config_t cfg = twoStaticTexts("w");
    cfg.background_color = helpers::Color("#0a0b0cff");
    canvas.loadFromAppConfig(cfg);
    check(canvas.exportAppConfig().background_color == helpers::Color("#0a0b0cff"),
          "an 8-digit background survives load -> export, got '" +
              canvas.exportAppConfig().background_color.value() + "'");
}

// --------------------------------------------------- the properties panel Apply
//
// Apply reads the form back into the config. Build and read used to be two
// independent walks joined only by the "field:<path>" objectNames, so a field
// that fell out of step stopped round-tripping in silence. The read now walks
// the widget subtree the build produced. These exercise the shapes that
// convention was worst at: a scalar, a vector, and a vector whose elements are
// themselves composite editors.

// Presses the page's Apply button, which is the only QPushButton whose text is
// "Apply" (the others are the vector's Add/Remove and the colour pickers).
bool pressApply(PropertiesPanel& panel)
{
    for (QPushButton* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == "Apply")
        {
            button->click();
            return true;
        }
    }
    return false;
}

void testApplyWritesScalarFieldsBack()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);
    QObject::connect(&canvas, &Canvas::selectionChanged, &panel, &PropertiesPanel::setSelectedWidget);

    canvas.loadFromAppConfig(twoStaticTexts("w"));
    const auto frames = canvas.frames();
    if (frames.empty())
    {
        check(false, "expected a frame");
        return;
    }
    canvas.selectFrame(frames[0]);

    auto* text = panel.findChild<QLineEdit*>("field:text");
    if (text == nullptr)
    {
        check(false, "the form has a text field");
        return;
    }
    text->setText("applied");
    check(pressApply(panel), "the page has an Apply button");

    check(std::get<StaticTextConfig_t>(frames[0]->config()).text == "applied",
          "Apply wrote the edited text back, got '" +
              std::get<StaticTextConfig_t>(frames[0]->config()).text + "'");
}

// A vector of a leaf type. Each element's editor is a plain spin box, and the
// count can change between build and read.
void testApplyRoundTripsAVectorField()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);
    QObject::connect(&canvas, &Canvas::selectionChanged, &panel, &PropertiesPanel::setSelectedWidget);

    app_config_t app;
    app.name = "vec";
    app.width = 640;
    app.height = 480;
    widget_config_t wc;
    wc.type = Mercedes190ESpeedometer::kWidgetType;
    wc.id = "speedo";
    wc.width = 200;
    wc.height = 200;
    Mercedes190ESpeedometerConfig_t sc;
    sc.shift_box_markers = {30, 60, 90};
    wc.config = sc;
    app.widgets.push_back(wc);

    canvas.loadFromAppConfig(app);
    const auto frames = canvas.frames();
    if (frames.empty())
    {
        check(false, "expected a frame");
        return;
    }
    canvas.selectFrame(frames[0]);

    // Untouched, the values have to survive an Apply unchanged.
    check(pressApply(panel), "the page has an Apply button");
    {
        const auto& markers =
            std::get<Mercedes190ESpeedometerConfig_t>(frames[0]->config()).shift_box_markers;
        check(markers == std::vector<uint16_t>({30, 60, 90}),
              "an untouched vector survives Apply, got " + std::to_string(markers.size()) +
                  " elements");
    }

    // Now edit one element in place.
    auto* second = panel.findChild<QSpinBox*>("field:shift_box_markers[1]");
    if (second == nullptr)
    {
        check(false, "the form has an editor for the second element");
        return;
    }
    second->setValue(65);
    check(pressApply(panel), "Apply again");
    {
        const auto& markers =
            std::get<Mercedes190ESpeedometerConfig_t>(frames[0]->config()).shift_box_markers;
        check(markers == std::vector<uint16_t>({30, 65, 90}),
              "Apply wrote the edited element back");
    }
}

// A vector whose elements are composite editors (a line edit plus a colour
// picker), and whose length changes after the form was built. Reading by
// position into a list captured at build time could not have survived this.
void testApplyFollowsVectorAddAndRemove()
{
    Canvas canvas;
    PropertiesPanel panel;
    panel.setCanvas(&canvas);
    QObject::connect(&canvas, &Canvas::selectionChanged, &panel, &PropertiesPanel::setSelectedWidget);

    app_config_t app;
    app.name = "colors";
    app.width = 640;
    app.height = 480;
    widget_config_t wc;
    wc.type = BackgroundRectWidget::kWidgetType;
    wc.id = "bg";
    wc.width = 100;
    wc.height = 100;
    BackgroundRectConfig_t bc;
    bc.colors = {helpers::Color("#111111"), helpers::Color("#222222")};
    wc.config = bc;
    app.widgets.push_back(wc);

    canvas.loadFromAppConfig(app);
    const auto frames = canvas.frames();
    if (frames.empty())
    {
        check(false, "expected a frame");
        return;
    }
    canvas.selectFrame(frames[0]);

    const auto colours = [&]
    { return std::get<BackgroundRectConfig_t>(frames[0]->config()).colors; };

    check(pressApply(panel), "the page has an Apply button");
    check(colours().size() == 2 && colours()[0] == helpers::Color("#111111") &&
              colours()[1] == helpers::Color("#222222"),
          "two composite elements survive an untouched Apply");

    // Add a row, then fill it in.
    QPushButton* add = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == "Add") add = button;
    }
    if (add == nullptr)
    {
        check(false, "the vector editor has an Add button");
        return;
    }
    add->click();

    auto* third = panel.findChild<QLineEdit*>("field:colors[2]");
    if (third == nullptr)
    {
        check(false, "the added row has an editor");
        return;
    }
    third->setText("#333333");
    check(pressApply(panel), "Apply after Add");
    check(colours().size() == 3 && colours()[2] == helpers::Color("#333333"),
          "Apply picked up the row added after the form was built, got " +
              std::to_string(colours().size()) + " colours");

    // Removing the MIDDLE row. Each row carries its own remove button, so the
    // entries after it keep their values -- the single global "remove the last
    // one" could only drop entries off the end, which meant retyping everything
    // after the one you actually wanted gone.
    std::vector<QPushButton*> removes;
    for (QPushButton* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == "✕") removes.push_back(button);
    }
    check(removes.size() == 3, "every row carries its own remove button, got " +
                                   std::to_string(removes.size()));
    if (removes.size() != 3)
    {
        return;
    }

    removes[1]->click();
    check(pressApply(panel), "Apply after removing the middle row");
    check(colours().size() == 2, "the removed row is gone, got " +
                                     std::to_string(colours().size()) + " colours");
    check(colours().size() == 2 && colours()[0] == helpers::Color("#111111") &&
              colours()[1] == helpers::Color("#333333"),
          "the rows either side of it kept their own values");
}

// ------------------------------------------------------ undo keeps its widgets
//
// Restoring a history entry used to reload the whole document: destroy every
// widget, rebuild every widget. Undoing a nudge of one static_text therefore tore
// down and rebuilt the CarPlay widget sitting next to it -- three zenoh
// subscriptions, a decoder and an audio sink -- with a visible glitch and a
// stream that has to re-sync.
//
// Pointer identity is the assertion, because it is the thing that was not true
// before and needs no instrumentation to check.
void testUndoingAMoveDoesNotRebuildWidgets()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    auto frames = canvas.frames();
    if (frames.size() != 2)
    {
        check(false, "expected two frames");
        return;
    }
    SelectionFrame* const moved = frames[0];
    SelectionFrame* const untouched = frames[1];
    QWidget* const moved_child = moved->child();
    QWidget* const untouched_child = untouched->child();
    const QPoint origin = moved->pos();

    {
        const auto tx = canvas.edit();
        moved->move(origin + QPoint(40, 40));
    }
    check(canvas.undo(), "the move is undone");

    frames = canvas.frames();
    check(frames.size() == 2, "both frames are still there");
    check(frames.size() == 2 && frames[0] == moved && frames[1] == untouched,
          "undo kept the frames themselves, in order");
    check(moved->child() == moved_child,
          "the moved widget was not rebuilt -- a move changes no configuration");
    check(untouched->child() == untouched_child, "the widget that was not touched was not rebuilt");
    check(moved->pos() == origin, "and the move was still undone");
}

// A config change does have to rebuild -- that is the one case where the live
// widget genuinely cannot be reused -- but only the widget that changed.
void testUndoingAConfigChangeRebuildsOnlyThatWidget()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    auto frames = canvas.frames();
    if (frames.size() != 2)
    {
        check(false, "expected two frames");
        return;
    }
    SelectionFrame* const changed = frames[0];
    SelectionFrame* const untouched = frames[1];
    QWidget* const changed_child = changed->child();
    QWidget* const untouched_child = untouched->child();

    {
        const auto tx = canvas.edit();
        StaticTextConfig_t sc = std::get<StaticTextConfig_t>(changed->config());
        sc.text = "edited";
        check(changed->applyConfig(sc), "the config is applied");
    }
    check(canvas.undo(), "the config change is undone");

    check(std::get<StaticTextConfig_t>(changed->config()).text == "first",
          "the configuration is back, got '" +
              std::get<StaticTextConfig_t>(changed->config()).text + "'");
    check(changed->child() != changed_child, "the widget whose config changed was rebuilt");
    check(untouched->child() == untouched_child, "the other widget was left alone");
}

// Adds and deletes still have to work through the same diff.
void testUndoRestoresAddedAndDeletedWidgets()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));
    canvas.clearHistory();

    SelectionFrame* const survivor = canvas.frames()[1];
    QWidget* const survivor_child = survivor->child();

    canvas.addWidget(widget_type_t::value_readout, QPoint(300, 300));
    check(canvas.frames().size() == 3, "the widget was added");
    check(canvas.undo(), "the add is undone");
    check(canvas.frames().size() == 2, "undoing the add removed it");
    check(canvas.frames()[1] == survivor && survivor->child() == survivor_child,
          "undoing an add left the other widgets alone");

    check(canvas.removeFrame(canvas.frames()[0]), "the first widget is deleted");
    check(canvas.frames().size() == 1, "the delete took effect");
    check(canvas.undo(), "the delete is undone");
    check(canvas.frames().size() == 2, "undoing the delete brought it back");
    check(std::get<StaticTextConfig_t>(canvas.frames()[0]->config()).text == "first",
          "the restored widget came back with its configuration");
}

// --------------------------------------------------- clamped preview, exact save
//
// widget_factory clamps a config into something drawable before the widget is
// built. The editor used to skip that entirely -- createWidgetFromConfig was
// called only by MainWindow -- so an out-of-range field previewed one way in the
// editor and drew another way in the dashboard, with the editor being the
// optimistic one.
//
// It now goes through the same factory, which raises the opposite risk: if the
// export read the config back off the live widget, saving would write the
// clamped value over whatever the user actually put in the file. The frame keeps
// the configured value for exactly this reason.
void testThePreviewIsClampedButTheSaveIsNot()
{
    // redline_rpm clamps into [0, max_rpm]; 20000 against a 9000 max is out of
    // range by a mile and will be pulled down.
    MotecC125TachometerConfig_t cfg;
    cfg.max_rpm = 9000;
    cfg.redline_rpm = 20000;

    app_config_t app;
    app.name = "clamping";
    app.width = 640;
    app.height = 480;
    widget_config_t wc;
    wc.type = MotecC125Tachometer::kWidgetType;
    wc.id = "tach";
    wc.width = 200;
    wc.height = 200;
    wc.config = cfg;
    app.widgets.push_back(wc);

    Canvas canvas;
    canvas.loadFromAppConfig(app);

    const auto frames = canvas.frames();
    if (frames.size() != 1 || frames[0]->child() == nullptr)
    {
        check(false, "expected one frame with a preview widget");
        return;
    }

    auto* widget = qobject_cast<MotecC125Tachometer*>(frames[0]->child());
    if (widget == nullptr)
    {
        check(false, "the preview is a MotecC125Tachometer");
        return;
    }

    check(widget->getConfig().redline_rpm <= widget->getConfig().max_rpm,
          "the preview widget was built from a clamped config, got redline " +
              std::to_string(widget->getConfig().redline_rpm) + " against max " +
              std::to_string(widget->getConfig().max_rpm));

    const app_config_t out = canvas.exportAppConfig();
    if (out.widgets.size() != 1)
    {
        check(false, "expected one widget back");
        return;
    }
    check(std::get<MotecC125TachometerConfig_t>(out.widgets[0].config).redline_rpm == 20000,
          "the export keeps the configured value, not the clamped one, got " +
              std::to_string(
                  std::get<MotecC125TachometerConfig_t>(out.widgets[0].config).redline_rpm));
}

// ------------------------------------------------------- generated equality

void testReflectedStructsCompareByValue()
{
    app_config_t a = twoStaticTexts("w");
    app_config_t b = twoStaticTexts("w");
    check(a == b, "two identically built configs compare equal");

    b.name = "different";
    check(!(a == b), "a differing window name compares unequal");

    b = twoStaticTexts("w");
    b.widgets[1].x = 999;
    check(!(a == b), "a differing widget position compares unequal");

    b = twoStaticTexts("w");
    StaticTextConfig_t sc = std::get<StaticTextConfig_t>(b.widgets[0].config);
    sc.text = "changed";
    b.widgets[0].config = sc;
    check(!(a == b), "a differing nested widget config compares unequal");

    // The variant's own comparison has to notice a change of alternative, not
    // just a change of value inside one.
    b = twoStaticTexts("w");
    b.widgets[0].config = std::monostate{};
    check(!(a == b), "a differing config alternative compares unequal");
}

// ------------------------------------------------------------ byte stability
//
// Every config we ship, opened in the editor and exported again, has to come
// back identical. Not "close enough": identical, because the editor's Save
// writes exactly this and overwrites the user's file with it.
//
// The corpus is the real configs/ directory rather than a fixture, so a config
// that grows a field the editor cannot carry fails here rather than in someone's
// working tree. Compared as emitted YAML, which is what actually gets written --
// the source file is not comparable directly, since it carries comments and its
// own key order.
//
// This is the guard for moving the document out of the widget tree: geometry and
// config are read back off live QWidgets today, so any mutation path that forgets
// to write back is a silent loss on save, and this is what catches it.
void testShippedConfigsSurviveTheEditorUnchanged()
{
    const std::filesystem::path dir{DASHBOARD_CONFIG_DIR};

    std::error_code ec;
    auto it = std::filesystem::directory_iterator(dir, ec);
    if (ec)
    {
        check(false, "cannot read the config directory " + dir.string() + ": " + ec.message());
        return;
    }

    std::vector<std::filesystem::path> configs;
    for (const auto& entry : it)
    {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml")
        {
            configs.push_back(entry.path());
        }
    }
    std::sort(configs.begin(), configs.end());

    // A corpus that silently became empty would make this test pass forever.
    check(!configs.empty(), "there is at least one shipped config to check");

    for (const auto& path : configs)
    {
        const auto loaded = load_app_config(path.string());
        if (!loaded)
        {
            check(false, path.filename().string() + " loads");
            continue;
        }

        Canvas canvas;
        canvas.loadFromAppConfig(*loaded);

        const std::string before = toYaml(*loaded);
        const std::string after = toYaml(canvas.exportAppConfig());
        check(before == after,
              path.filename().string() + " survives load -> export unchanged\n--- before ---\n" +
                  before + "\n--- after ---\n" + after);
    }
}

// A drag carrying text that is not a widget type used to reach a throwing
// lookup inside a Qt event handler and terminate the editor.
void testUnknownDropPayloadIsRefused()
{
    Canvas canvas;
    canvas.loadFromAppConfig(twoStaticTexts("w"));

    // addWidget is what dropEvent calls once it has resolved a type; the refusal
    // itself is dragEnterEvent's job and is covered by the type lookup below.
    check(canvas.addWidget(widget_type_t::unknown, QPoint(0, 0)) == nullptr,
          "an unknown widget type is refused rather than constructed");
    check(!reflection::enum_traits<widget_type_t>::try_from_string("https://example.com").has_value(),
          "arbitrary dragged text does not resolve to a widget type");
    check(reflection::enum_traits<widget_type_t>::try_from_string("static_text").has_value(),
          "a real palette payload still resolves");
    check(canvas.exportAppConfig().widgets.size() == 2, "the canvas is unchanged by a refused drop");
}

}  // namespace

int main(int argc, char** argv)
{
    // Must be set before QApplication: these tests construct real widgets and
    // must not need a display.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testWindowIdentitySurvivesTheEditor();
    testWidgetIdentityAndConfigSurviveTheEditor();
    testSelectingASecondWidgetOfTheSameTypeShowsItsOwnValues();
    testDeletingTheSelectionLeavesAConsistentCanvas();
    testDerivedNamesAreNotReusedAfterADelete();
    testAnUnknownWidgetDoesNotShiftLaterNames();
    testUndoAndRedoWalkTheEditHistory();
    testNamesSurviveAnUndo();
    testNoOpEditsAreNotRecorded();
    testDirtyTracksTheLastSave();
    testWindowPropertiesAreUndoable();
    testWindowNameIsUndoable();
    testTypingAWindowFieldIsOneHistoryEntry();
    testAnOpenWindowEditDoesNotSwallowTheNextEdit();
    testHalfTypedColoursAreNotApplied();
    testBackgroundSurvivesTheCanvas();
    testApplyWritesScalarFieldsBack();
    testApplyRoundTripsAVectorField();
    testApplyFollowsVectorAddAndRemove();
    testUndoingAMoveDoesNotRebuildWidgets();
    testUndoingAConfigChangeRebuildsOnlyThatWidget();
    testUndoRestoresAddedAndDeletedWidgets();
    testThePreviewIsClampedButTheSaveIsNot();
    testReflectedStructsCompareByValue();
    testShippedConfigsSurviveTheEditorUnchanged();
    testUnknownDropPayloadIsRefused();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
