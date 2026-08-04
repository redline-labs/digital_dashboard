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

#include <cstdio>
#include <string>

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
    testUnknownDropPayloadIsRefused();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
