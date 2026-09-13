// The YAML codec: does a config survive being written and read back?
//
// This exists because a load/save cycle through the editor was quietly changing
// configs. The window name was replaced with a hardcoded one, and the
// speedometer's std::vector<uint8_t> markers were emitted as characters -- 28
// became a raw 0x1c control byte in the file -- because yaml-cpp treats an
// 8-bit integer as a char. Both were invisible until someone diffed a file.
//
// The widget sweep is driven by DASHBOARD_WIDGET_TABLE, so a widget added later is
// covered here without anyone remembering to come back.

#include "dashboard/app_config.h"
#include "editor/widget_registry.h"

#include <yaml-cpp/yaml.h>

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

// Not named `emit`: Qt defines that as a macro, and the collision is a
// spectacularly confusing pile of syntax errors.
std::string toYaml(const app_config_t& cfg)
{
    YAML::Emitter out;
    out << YAML::convert<app_config_t>::encode(cfg);
    return out.c_str();
}

app_config_t reload(const app_config_t& cfg)
{
    return YAML::Load(toYaml(cfg)).as<app_config_t>();
}

// ---------------------------------------------------------------- window keys

void testWindowFieldsSurvive()
{
    app_config_t cfg;
    cfg.name = "instrument_cluster";
    cfg.width = 1200;
    cfg.height = 450;
    cfg.background_color = helpers::Color("#123456");

    const app_config_t back = reload(cfg);

    // The editor used to export with a hardcoded name, so every config it
    // touched came back called "editor_window".
    check(back.name == "instrument_cluster", "window name survives a round trip");
    check(back.width == 1200, "window width survives a round trip");
    check(back.height == 450, "window height survives a round trip");
    check(back.background_color.value() == "#123456", "background colour survives a round trip");
}

// -------------------------------------------------------------- widget sweep

// One entry per widget type, each with its default config, so every alternative
// of the variant is encoded and decoded at least once.
void testEveryWidgetTypeSurvives()
{
    app_config_t cfg;
    cfg.name = "sweep";

#define ADD_WIDGET(enum_name, widget_class)                                    \
    {                                                               \
        widget_config_t wc;                                         \
        wc.type = widget_class::kWidgetType;                        \
        wc.id = std::string("id_") +                                \
                std::string(reflection::enum_to_string(widget_class::kWidgetType)); \
        wc.x = 11;                                                  \
        wc.y = 22;                                                  \
        wc.width = 33;                                              \
        wc.height = 44;                                             \
        wc.config = widget_class::config_t{};                       \
        cfg.widgets.push_back(wc);                                  \
    }

    DASHBOARD_WIDGET_TABLE(ADD_WIDGET)
#undef ADD_WIDGET

    const app_config_t back = reload(cfg);

    check(back.widgets.size() == cfg.widgets.size(),
          "every widget survives (" + std::to_string(back.widgets.size()) + " of " +
              std::to_string(cfg.widgets.size()) + ")");

    for (std::size_t i = 0; i < back.widgets.size() && i < cfg.widgets.size(); ++i)
    {
        const auto& before = cfg.widgets[i];
        const auto& after = back.widgets[i];
        const std::string what = std::string(reflection::enum_to_string(before.type));

        check(after.type == before.type, what + ": type survives");

        // An id is how the agent interface addresses a widget across a save and
        // reload; losing it silently breaks every selector.
        check(after.id == before.id, what + ": id survives");

        check(after.x == before.x && after.y == before.y, what + ": position survives");
        check(after.width == before.width && after.height == before.height, what + ": size survives");
        check(after.config.index() == before.config.index(),
              what + ": the config variant holds the same alternative");
    }
}

// An empty id must stay absent rather than becoming an empty string, so a config
// that never had one is not rewritten with `id: ""` on every save.
void testEmptyIdIsOmitted()
{
    app_config_t cfg;
    widget_config_t wc;
    wc.type = StaticTextWidget::kWidgetType;
    wc.config = StaticTextWidget::config_t{};
    cfg.widgets.push_back(wc);

    const std::string text = toYaml(cfg);
    check(text.find("id:") == std::string::npos, "an unset id is not written out");
    check(reload(cfg).widgets.at(0).id.empty(), "an unset id stays unset");
}

// ------------------------------------------------------------ numeric fidelity

// yaml-cpp encodes an 8-bit integer as a character, so a uint8_t config field
// round-tripped through the file as text. This pins the values, not the types:
// if someone reintroduces a uint8_t here the numbers come back wrong.
void testNumericListsStayNumeric()
{
    app_config_t cfg;
    widget_config_t wc;
    wc.type = Mercedes190ESpeedometer::kWidgetType;

    Mercedes190ESpeedometerConfig_t speedo;
    speedo.shift_box_markers = {28, 54, 87};  // 0x1c, '6', 'W' as characters
    speedo.odometer_value = 123456;
    speedo.max_speed = 180;
    wc.config = speedo;
    cfg.widgets.push_back(wc);

    const std::string text = toYaml(cfg);
    check(text.find("28") != std::string::npos, "a marker is written as the number 28");
    check(text.find("\\x1c") == std::string::npos && text.find('\x1c') == std::string::npos,
          "no marker is written as a control character");

    const app_config_t reloaded = reload(cfg);
    const auto& back = std::get<Mercedes190ESpeedometerConfig_t>(reloaded.widgets.at(0).config);
    check(back.shift_box_markers == speedo.shift_box_markers, "marker values survive unchanged");
    check(back.odometer_value == 123456, "a large odometer value survives");
    check(back.max_speed == 180, "max_speed survives");
}

// A string field with characters YAML gives meaning to must come back intact.
void testAwkwardStringsSurvive()
{
    app_config_t cfg;
    cfg.name = "has: a colon";

    widget_config_t wc;
    wc.type = StaticTextWidget::kWidgetType;
    StaticTextConfig_t text_cfg;
    text_cfg.text = "100%  #not-a-comment  \"quoted\"";
    wc.config = text_cfg;
    cfg.widgets.push_back(wc);

    const app_config_t back = reload(cfg);
    check(back.name == "has: a colon", "a name containing a colon survives");
    check(std::get<StaticTextConfig_t>(back.widgets.at(0).config).text == text_cfg.text,
          "text with YAML metacharacters survives");
}

// A widget with no `config:` block loads, and loads as that widget's defaults.
//
// validate_widget() calls this legal and only warns, but the decoder used to read
// node["config"] unconditionally -- and yaml-cpp throws on an undefined node, so
// the exception escaped to load_app_config and took the whole file down. Nothing
// shipped in configs/ omits the block, so nothing caught it.
//
// The variant must hold the widget's own config, not std::monostate: monostate
// means "unknown widget type, construct nothing" to widget_factory.h, which would
// have shown the widget in the editor and silently dropped it from the dashboard.
void testMissingConfigBlockLoadsDefaults()
{
    const auto node = YAML::Load(R"(
name: "no_config_block"
width: 800
height: 400
widgets:
  - type: static_text
    id: bare
    x: 10
    y: 20
    width: 200
    height: 60
)");

    app_config_t cfg;
    bool threw = false;
    try
    {
        cfg = node.as<app_config_t>();
    }
    catch (const std::exception&)
    {
        threw = true;
    }

    check(!threw, "a widget with no config block decodes instead of throwing");
    if (threw)
    {
        return;
    }

    check(cfg.widgets.size() == 1, "the config-less widget is kept");
    const widget_config_t& wc = cfg.widgets.at(0);
    check(wc.type == StaticTextWidget::kWidgetType, "its type is decoded from `type:`");
    check(wc.x == 10 && wc.y == 20, "its placement keys are decoded");
    check(std::holds_alternative<StaticTextConfig_t>(wc.config),
          "its config variant holds the widget's own type, not monostate");

    if (std::holds_alternative<StaticTextConfig_t>(wc.config))
    {
        check(std::get<StaticTextConfig_t>(wc.config).text == StaticTextConfig_t{}.text,
              "the decoded config equals a default-constructed one");
    }
}

// A reflected type converts without being registered anywhere.
//
// These two are declared here and named in no list: not in app_config.h, not in
// the widget table, nowhere. If conversion still works, adding a nested struct or
// an enum to a widget config needs no central edit -- which is the whole point
// of deriving the converters from the reflection traits.
//
// The failure this guards against does not look like a missing registration. It
// is an "implicit instantiation of undefined template" from inside yaml-cpp,
// blaming whichever header first instantiated it.
REFLECT_ENUM(TestOnlyFace, flat, raised)

REFLECT_STRUCT(test_only_nested_t,
    (int32_t, depth, -7),
    (TestOnlyFace, face, TestOnlyFace::raised)
)

REFLECT_STRUCT(test_only_outer_t,
    (std::string, label, "unset"),
    (test_only_nested_t, nested, {})
)

void testReflectedTypesNeedNoRegistration()
{
    test_only_outer_t original;
    original.label = "written by nobody's macro";
    original.nested.depth = 42;
    original.nested.face = TestOnlyFace::flat;

    YAML::Emitter out;
    out << YAML::convert<test_only_outer_t>::encode(original);

    const YAML::Node node = YAML::Load(out.c_str());
    const auto back = node.as<test_only_outer_t>();

    check(back.label == original.label, "an unregistered struct's field survives");
    check(back.nested.depth == 42, "an unregistered nested struct survives");
    check(back.nested.face == TestOnlyFace::flat, "an unregistered enum survives");

    // The enum has to go out as its name, not as its underlying integer --
    // otherwise the file is unreadable and the validator cannot name the
    // alternatives back to the author.
    check(node["nested"]["face"].as<std::string>() == "flat",
          "an unregistered enum is written as its name");
}

// ------------------------------------------------------------- window lists

std::string toYaml(const dashboard_config_t& cfg)
{
    YAML::Emitter out;
    out << YAML::convert<dashboard_config_t>::encode(cfg);
    return out.c_str();
}

// Every config written before there could be two windows is flat, and must load
// as one window without anybody touching it.
void testFlatFormLoadsAsOneWindow()
{
    const auto doc = YAML::Load(R"(
name: instrument_cluster
width: 1200
height: 450
background_color: "#000000"
widgets: []
)").as<dashboard_config_t>();

    check(doc.windows.size() == 1, "a flat config is one window");
    check(doc.name.empty(), "a flat config's name is the window's, not the document's");
    if (doc.windows.size() == 1)
    {
        const app_config_t& window = doc.windows.front();
        check(window.name == "instrument_cluster", "the flat name lands on the window");
        check(window.width == 1200 && window.height == 450, "the flat size lands on the window");
        check(window.display == display_role_t::primary, "a window with no display is primary");
        check(window.scale == scale_mode_t::fit, "a window with no scale fits");
    }
}

void testWindowListLoads()
{
    const auto doc = YAML::Load(R"(
name: mercedes_190e
windows:
  - name: cluster
    width: 1200
    height: 450
    widgets: []
  - name: carplay
    display: secondary
    scale: none
    width: 800
    height: 480
    widgets:
      - type: static_text
        config: {}
)").as<dashboard_config_t>();

    check(doc.name == "mercedes_190e", "the document name is read from beside `windows:`");
    check(doc.windows.size() == 2, "both windows load");
    if (doc.windows.size() == 2)
    {
        check(doc.windows[0].display == display_role_t::primary, "the first window defaults to primary");
        check(doc.windows[1].display == display_role_t::secondary, "`display: secondary` is read");
        check(doc.windows[1].scale == scale_mode_t::none, "`scale: none` is read");
        check(doc.windows[1].widgets.size() == 1, "a window's widgets are its own");
    }
}

// The editor saves whatever it holds, so the form it picks is what decides
// whether every shipped config churns on its first save.
void testTheWrittenFormIsTheSmallestThatSaysEverything()
{
    dashboard_config_t one;
    one.windows.push_back(app_config_t{});
    one.windows[0].name = "cluster";
    const std::string flat = toYaml(one);
    check(flat.find("windows:") == std::string::npos, "one default window is written flat");
    check(flat.find("display:") == std::string::npos && flat.find("scale:") == std::string::npos,
          "default display and scale are not written out");

    dashboard_config_t secondary = one;
    secondary.windows[0].display = display_role_t::secondary;
    const std::string listed = toYaml(secondary);
    check(listed.find("windows:") != std::string::npos,
          "a window the flat form cannot place is written as a list");
    check(listed.find("display: secondary") != std::string::npos, "a non-default display is written");

    dashboard_config_t named = one;
    named.name = "document";
    check(toYaml(named).find("windows:") != std::string::npos,
          "a document name forces the list form, which is the only one with room for it");

    dashboard_config_t two = one;
    two.windows.push_back(app_config_t{});
    two.windows[1].name = "carplay";
    two.windows[1].display = display_role_t::secondary;
    two.windows[1].scale = scale_mode_t::none;

    const auto back = YAML::Load(toYaml(two)).as<dashboard_config_t>();
    check(back == two, "a two-window document survives a round trip:\n" + toYaml(two));
}

}  // namespace

int main()
{
    testFlatFormLoadsAsOneWindow();
    testWindowListLoads();
    testTheWrittenFormIsTheSmallestThatSaysEverything();
    testWindowFieldsSurvive();
    testEveryWidgetTypeSurvives();
    testEmptyIdIsOmitted();
    testNumericListsStayNumeric();
    testAwkwardStringsSurvive();
    testMissingConfigBlockLoadsDefaults();
    testReflectedTypesNeedNoRegistration();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
