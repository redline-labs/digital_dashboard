// The YAML codec: does a config survive being written and read back?
//
// This exists because a load/save cycle through the editor was quietly changing
// configs. The window name was replaced with a hardcoded one, and the
// speedometer's std::vector<uint8_t> markers were emitted as characters -- 28
// became a raw 0x1c control byte in the file -- because yaml-cpp treats an
// 8-bit integer as a char. Both were invisible until someone diffed a file.
//
// The widget sweep is driven by FOR_EACH_WIDGET, so a widget added later is
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

#define ADD_WIDGET(widget_class)                                    \
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

    FOR_EACH_WIDGET(ADD_WIDGET)
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

}  // namespace

int main()
{
    testWindowFieldsSurvive();
    testEveryWidgetTypeSurvives();
    testEmptyIdIsOmitted();
    testNumericListsStayNumeric();
    testAwkwardStringsSurvive();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
