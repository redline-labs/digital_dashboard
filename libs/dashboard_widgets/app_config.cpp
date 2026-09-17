#include "dashboard/app_config.h"

#include "dashboard/page_command.h"
#include "pub_sub/topic_key.h"

#include <spdlog/spdlog.h>

#include <map>
#include <optional>
#include <set>
#include <string>

namespace {

using config_codec::Issue;

// What the walk learns about the whole document on the way through, for the
// rules that span widgets: a button naming a page_stack in another window, two
// stacks claiming one topic.
struct ValidationContext
{
    struct Container
    {
        std::string path;
        std::set<std::string> pages;
    };
    std::map<std::string, Container> containers;

    // A page command somewhere in a widget's config, checked once every stack is
    // known. `path` is the command struct's own path.
    struct CommandRef
    {
        std::string path;
        std::string target;
        std::string action;
        std::string page;
    };
    std::vector<CommandRef> commands;

    std::vector<std::string> carplay_paths;

    // 0 for a window's widgets, 1 inside a page.
    int depth = 0;
};

std::optional<std::string> scalarText(const YAML::Node& node)
{
    if (!node || !node.IsScalar())
    {
        return std::nullopt;
    }
    return node.as<std::string>();
}

bool scalarTrue(const YAML::Node& node)
{
    try
    {
        return node && node.IsScalar() && node.as<bool>();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// A command struct, read leniently: its fields' types are validateStruct's job,
// this only collects what the cross-widget check needs.
void collectCommand(const YAML::Node& command, const std::string& path, ValidationContext& ctx)
{
    if (!command || !command.IsMap())
    {
        return;
    }
    ctx.commands.push_back({path, scalarText(command["target"]).value_or(""),
                            scalarText(command["action"]).value_or("next"),
                            scalarText(command["page"]).value_or("")});
}

void validateWidget(const YAML::Node& node, const std::string& prefix, std::size_t index,
                    std::vector<Issue>& issues, ValidationContext& ctx);

// Integer placement key, or its default when absent or unreadable (reported
// elsewhere).
int placement(const YAML::Node& widget, const char* key, int fallback)
{
    try
    {
        return widget[key] ? widget[key].as<int>() : fallback;
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

// A page_stack's own rules: an id that can name topics, pages that exist and
// are named once, and widgets inside them that are not themselves stacks.
void validatePageStack(const YAML::Node& node, const std::string& path, std::vector<Issue>& issues,
                       ValidationContext& ctx)
{
    const std::optional<std::string> id = scalarText(node["id"]);
    if (!id || id->empty())
    {
        issues.push_back({Issue::Severity::error, path + ".id",
                          "a page_stack needs an id: it names the stack's topics, "
                          "dashboard/pages/<id>/command and /state"});
    }
    else if (id->find('/') != std::string::npos)
    {
        // Still a valid key, but no longer one segment: dashboard/pages/*/state
        // would miss this stack, and its topics would look like another's.
        issues.push_back({Issue::Severity::error, path + ".id",
                          "'" + *id + "' contains '/'; a page_stack id is one segment of its topic keys"});
    }
    else if (const std::string problem = pub_sub::topicKeyProblem(dashboard::pageCommandKey(*id)); !problem.empty())
    {
        issues.push_back({Issue::Severity::error, path + ".id",
                          "'" + *id + "' cannot name the stack's topics: " + problem});
    }
    else if (const auto it = ctx.containers.find(*id); it != ctx.containers.end())
    {
        issues.push_back({Issue::Severity::error, path + ".id",
                          "'" + *id + "' is already the id of the page_stack at " + it->second.path +
                              "; the two would share one command topic"});
    }

    ValidationContext::Container container{path, {}};
    const YAML::Node pages = node["pages"];
    if (!pages)
    {
        issues.push_back({Issue::Severity::error, path + ".pages",
                          "missing; a page_stack needs at least one page"});
    }
    else if (!pages.IsSequence() || pages.size() == 0)
    {
        issues.push_back({Issue::Severity::error, path + ".pages", "expected a non-empty list of pages"});
    }
    else
    {
        const int stack_w = placement(node, "width", 100);
        const int stack_h = placement(node, "height", 100);

        ++ctx.depth;
        for (std::size_t p = 0; p < pages.size(); ++p)
        {
            const std::string page_path = path + ".pages[" + std::to_string(p) + "]";
            const YAML::Node page = pages[p];
            if (!page.IsMap())
            {
                issues.push_back({Issue::Severity::error, page_path, "expected a mapping"});
                continue;
            }

            for (const auto& entry : page)
            {
                const std::string key = entry.first.as<std::string>();
                if (key != "name" && key != "in_cycle" && key != "widgets")
                {
                    issues.push_back({Issue::Severity::warning, page_path + "." + key,
                                      "unknown key, ignored; a page has name, in_cycle and widgets"});
                }
            }

            const std::optional<std::string> name = scalarText(page["name"]);
            if (!name || name->empty())
            {
                issues.push_back({Issue::Severity::error, page_path + ".name",
                                  "missing; commands and triggers find a page by its name"});
            }
            else if (!container.pages.insert(*name).second)
            {
                issues.push_back({Issue::Severity::error, page_path + ".name",
                                  "'" + *name + "' is already a page of this page_stack"});
            }

            if (page["in_cycle"])
            {
                try
                {
                    (void)page["in_cycle"].as<bool>();
                }
                catch (const std::exception&)
                {
                    issues.push_back({Issue::Severity::error, page_path + ".in_cycle", "expected true or false"});
                }
            }

            const YAML::Node widgets = page["widgets"];
            if (!widgets)
            {
                issues.push_back({Issue::Severity::warning, page_path + ".widgets", "missing; the page will be empty"});
                continue;
            }
            if (!widgets.IsSequence())
            {
                issues.push_back({Issue::Severity::error, page_path + ".widgets", "expected a list"});
                continue;
            }
            for (std::size_t w = 0; w < widgets.size(); ++w)
            {
                validateWidget(widgets[w], page_path + ".", w, issues, ctx);

                // Placed relative to the stack, and clipped by it: a child that
                // overhangs is drawn cut off, which is legal but rarely meant.
                const YAML::Node child = widgets[w];
                if (child.IsMap())
                {
                    const int x = placement(child, "x", 0);
                    const int y = placement(child, "y", 0);
                    const int right = x + placement(child, "width", 100);
                    const int bottom = y + placement(child, "height", 100);
                    if (x < 0 || y < 0 || right > stack_w || bottom > stack_h)
                    {
                        issues.push_back({Issue::Severity::warning,
                                          page_path + ".widgets[" + std::to_string(w) + "]",
                                          "extends past its page_stack (" + std::to_string(stack_w) + "x" +
                                              std::to_string(stack_h) +
                                              "); positions in a page are relative to the stack, and the "
                                              "overhang is clipped"});
                    }
                }
            }
        }
        --ctx.depth;
    }

    const YAML::Node config = node["config"];
    if (config && config.IsMap())
    {
        if (const auto default_page = scalarText(config["default_page"]);
            default_page && !default_page->empty() && !container.pages.empty() && !container.pages.contains(*default_page))
        {
            issues.push_back({Issue::Severity::error, path + ".config.default_page",
                              "no page named '" + *default_page + "' in this page_stack"});
        }

        const YAML::Node triggers = config["triggers"];
        if (triggers && triggers.IsSequence())
        {
            for (std::size_t t = 0; t < triggers.size(); ++t)
            {
                const std::string trigger_path = path + ".config.triggers[" + std::to_string(t) + "]";
                const YAML::Node trigger = triggers[t];
                if (!trigger.IsMap())
                {
                    continue;  // validateStruct says so
                }
                if (scalarText(trigger["zenoh_key"]).value_or("").empty())
                {
                    issues.push_back({Issue::Severity::error, trigger_path + ".zenoh_key",
                                      "missing; a trigger has to watch a topic"});
                }
                if (scalarText(trigger["expression"]).value_or("").empty())
                {
                    issues.push_back({Issue::Severity::error, trigger_path + ".expression",
                                      "missing; say what makes it fire, e.g. bit(buttons1To8, 0)"});
                }
                if (scalarText(trigger["action"]).value_or("next") == "go_to")
                {
                    const std::string page_name = scalarText(trigger["page"]).value_or("");
                    if (page_name.empty())
                    {
                        issues.push_back({Issue::Severity::error, trigger_path + ".page", "go_to needs a page"});
                    }
                    else if (!container.pages.empty() && !container.pages.contains(page_name))
                    {
                        issues.push_back({Issue::Severity::error, trigger_path + ".page",
                                          "no page named '" + page_name + "' in this page_stack"});
                    }
                }
            }
        }
    }

    if (id && !id->empty() && !ctx.containers.contains(*id))
    {
        ctx.containers.emplace(*id, std::move(container));
    }
}

// Each page command collected on the walk, against the stacks the walk found.
void validateCommands(const ValidationContext& ctx, std::vector<Issue>& issues)
{
    for (const ValidationContext::CommandRef& command : ctx.commands)
    {
        if (command.target.empty())
        {
            issues.push_back({Issue::Severity::error, command.path + ".target",
                              "missing; name the id of the page_stack this changes"});
            continue;
        }

        const bool go_to = command.action == "go_to";
        if (go_to && command.page.empty())
        {
            issues.push_back({Issue::Severity::error, command.path + ".page", "go_to needs a page"});
        }

        const auto it = ctx.containers.find(command.target);
        if (it == ctx.containers.end())
        {
            // A warning, not an error: the stack may be in another process, and
            // the command goes on the bus either way.
            issues.push_back({Issue::Severity::warning, command.path + ".target",
                              "no page_stack with id '" + command.target + "' in this config; the command "
                              "is sent to " + dashboard::pageCommandKey(command.target) +
                              " and nothing here will act on it"});
            continue;
        }
        if (go_to && !command.page.empty() && !it->second.pages.contains(command.page))
        {
            issues.push_back({Issue::Severity::error, command.path + ".page",
                              "page_stack '" + command.target + "' has no page named '" + command.page + "'"});
        }
    }

    // The carplay node listens to one visibility topic. Two widgets publishing
    // on it contradict each other every second.
    for (std::size_t i = 1; i < ctx.carplay_paths.size(); ++i)
    {
        issues.push_back({Issue::Severity::warning, ctx.carplay_paths[i],
                          "a second carplay widget (the first is " + ctx.carplay_paths[0] +
                              "); both report visibility to the same node, and they will disagree"});
    }
}

// Validates one entry of the `widgets:` list. The per-widget `config:` block is
// a different struct for every `type:`, so the walk has to dispatch, and
// The widget table is what makes that automatic for a widget added later.
void validateWidget(const YAML::Node& node, const std::string& prefix, std::size_t index,
                    std::vector<Issue>& issues, ValidationContext& ctx)
{
    const std::string path = prefix + "widgets[" + std::to_string(index) + "]";

    if (!node.IsMap())
    {
        issues.push_back({Issue::Severity::error, path, "expected a mapping"});
        return;
    }

    // `type` is the one genuinely required key: without it there is no way to
    // know which config struct the `config:` block should be read as.
    if (!node["type"])
    {
        issues.push_back({Issue::Severity::error, path + ".type",
                          "missing; every widget entry needs a type"});
        return;
    }

    const std::string type_name = node["type"].as<std::string>();
    const auto type = reflection::enum_traits<widget_type_t>::try_from_string(type_name);
    if (!type || *type == widget_type_t::unknown)
    {
        // Built from the widget table rather than the enum, so `unknown` -- which
        // is an internal state, not something anyone should write in a file --
        // is not offered as a suggestion.
        std::string known;
#define KNOWN_TYPE(enum_name, widget_class)                                                             \
    if (!known.empty()) known += ", ";                                                       \
    known += std::string(reflection::enum_to_string(widget_class::kWidgetType));

        DASHBOARD_WIDGET_TABLE(KNOWN_TYPE)
#undef KNOWN_TYPE

        issues.push_back({Issue::Severity::error, path + ".type",
                          "unknown widget type '" + type_name + "'; expected one of: " + known});
        return;
    }

    // The placement keys, which live beside `type` rather than inside `config`.
    static constexpr std::string_view kPlacementKeys[] = {"type", "id", "x", "y", "width", "height", "config"};
    for (const auto& entry : node)
    {
        const std::string key = entry.first.as<std::string>();
        if (key == "pages")
        {
            // An error rather than "unknown key, ignored": what would be ignored
            // is a list of widgets, and dropping those silently is the bad outcome.
            if (*type != widget_type_t::page_stack)
            {
                issues.push_back({Issue::Severity::error, path + ".pages",
                                  "only a page_stack has pages; this is a " + type_name});
            }
            continue;
        }
        if (std::find(std::begin(kPlacementKeys), std::end(kPlacementKeys), key) == std::end(kPlacementKeys))
        {
            issues.push_back({Issue::Severity::warning, path + "." + key,
                              "unknown key, ignored"});
        }
    }

    switch (*type)
    {
        case widget_type_t::page_stack:
            if (ctx.depth > 0)
            {
                issues.push_back({Issue::Severity::error, path + ".type",
                                  "a page_stack cannot be inside another page_stack's page"});
                return;
            }
            validatePageStack(node, path, issues, ctx);
            break;
        case widget_type_t::page_button:
            if (node["config"] && node["config"].IsMap())
            {
                collectCommand(node["config"]["command"], path + ".config.command", ctx);
            }
            break;
        case widget_type_t::carplay:
            ctx.carplay_paths.push_back(path);
            if (node["config"] && node["config"].IsMap())
            {
                const YAML::Node return_button = node["config"]["return_button"];
                if (return_button && return_button.IsMap() && scalarTrue(return_button["enabled"]))
                {
                    collectCommand(return_button["command"], path + ".config.return_button.command", ctx);
                }
            }
            break;
        case widget_type_t::static_text:
        case widget_type_t::road_info:
        case widget_type_t::value_readout:
        case widget_type_t::segment_readout:
        case widget_type_t::center_bar:
        case widget_type_t::mercedes_190e_speedometer:
        case widget_type_t::mercedes_190e_tachometer:
        case widget_type_t::mercedes_190e_cluster_gauge:
        case widget_type_t::sparkline:
        case widget_type_t::background_rect:
        case widget_type_t::mercedes_190e_telltale:
        case widget_type_t::motec_c125_tachometer:
        case widget_type_t::motec_cdl3_tachometer:
        case widget_type_t::now_playing:
        case widget_type_t::carplay_nav:
        case widget_type_t::map:
        case widget_type_t::unknown:
            break;
    }

    if (!node["config"])
    {
        // Legal -- the widget takes its defaults -- but worth saying, because a
        // widget with no config is almost never what was meant.
        issues.push_back({Issue::Severity::warning, path + ".config",
                          "missing; the widget will use every default"});
        return;
    }

    const std::string cfg_path = path + ".config";
#define VALIDATE_CONFIG_CASE(enum_name, widget_class)                                                   \
    if (*type == widget_class::kWidgetType)                                                  \
    {                                                                                        \
        config_codec::detail::validateStruct<widget_class::config_t>(                   \
            node["config"], cfg_path, issues);                                               \
        return;                                                                              \
    }

    DASHBOARD_WIDGET_TABLE(VALIDATE_CONFIG_CASE)
#undef VALIDATE_CONFIG_CASE
}

// A scalar that must name one value of a reflected enum. Checked by name rather
// than through as<Enum>(), because yaml-cpp turns a failed conversion into a bare
// "bad conversion" that names neither the value nor the alternatives.
template <typename Enum>
void validateEnumKey(const YAML::Node& window, const char* key, const std::string& prefix,
                     std::vector<Issue>& issues)
{
    if (!window[key])
    {
        return;
    }

    const std::string path = prefix + key;
    if (!window[key].IsScalar())
    {
        issues.push_back({Issue::Severity::error, path, "expected a name, not a collection"});
        return;
    }

    const std::string text = window[key].as<std::string>();
    if (!reflection::enum_traits<Enum>::try_from_string(text))
    {
        issues.push_back({Issue::Severity::error, path,
                          "unknown value '" + text + "'; expected one of: " +
                              reflection::enum_traits<Enum>::known_values()});
    }
}

// One window's keys and widgets. `prefix` is "" for the flat form, where the
// window's keys sit at the top of the file, and "windows[N]." otherwise, so every
// path names exactly where in the file the problem is.
void validateWindow(const YAML::Node& window, const std::string& prefix, std::vector<Issue>& issues,
                    ValidationContext& ctx)
{
    // The window-level keys, validated against app_config_t itself. `widgets` is
    // handled separately below because its element type depends on `type`.
    static constexpr std::string_view kWindowKeys[] = {"name", "width", "height", "background_color",
                                                       "display", "scale", "widgets"};
    for (const auto& entry : window)
    {
        const std::string key = entry.first.as<std::string>();
        // `windows` belongs to the document, not the window. The flat form cannot
        // carry it (validate_app_config picks the form on its presence), so it is
        // only ever seen here when it is legitimate.
        if (prefix.empty() && key == "windows")
        {
            continue;
        }
        if (std::find(std::begin(kWindowKeys), std::end(kWindowKeys), key) == std::end(kWindowKeys))
        {
            issues.push_back({Issue::Severity::warning, prefix + key, "unknown key, ignored"});
        }
    }

    // The window's own scalars. These cannot go through validateStruct against
    // app_config_t, because its `widgets` field is a vector whose element type
    // depends on `type` -- so they are checked individually here.
    if (window["background_color"])
    {
        const std::string color = window["background_color"].as<std::string>();
        if (!helpers::Color::isValidFormat(color))
        {
            // This one goes straight into a Qt stylesheet, where an unparseable
            // value makes Qt drop the whole rule and the window keeps whatever
            // background it had. Silently.
            issues.push_back({Issue::Severity::error, prefix + "background_color",
                              "'" + color + "' is not a colour; expected #RGB, #RRGGBB or #RRGGBBAA"});
        }
    }

    for (const char* key : {"width", "height"})
    {
        if (!window[key]) continue;
        try
        {
            (void)window[key].as<uint16_t>();
        }
        catch (const std::exception& e)
        {
            issues.push_back({Issue::Severity::error, prefix + key, e.what()});
        }
    }

    validateEnumKey<display_role_t>(window, "display", prefix, issues);
    validateEnumKey<scale_mode_t>(window, "scale", prefix, issues);

    if (!window["widgets"])
    {
        issues.push_back({Issue::Severity::warning, prefix + "widgets", "missing; the window will be empty"});
        return;
    }

    if (!window["widgets"].IsSequence())
    {
        issues.push_back({Issue::Severity::error, prefix + "widgets", "expected a list"});
        return;
    }

    for (std::size_t i = 0; i < window["widgets"].size(); ++i)
    {
        validateWidget(window["widgets"][i], prefix, i, issues, ctx);
    }
}

// The `windows:` form: a document name and a list of windows, each validated as
// the flat form would be, plus the rules only a list can break.
void validateWindowList(const YAML::Node& root, std::vector<Issue>& issues, ValidationContext& ctx)
{
    for (const auto& entry : root)
    {
        const std::string key = entry.first.as<std::string>();
        if (key == "name" || key == "windows")
        {
            continue;
        }

        // A window key beside `windows:` is almost certainly a file half-way
        // through being converted, and which of the two was meant cannot be
        // guessed. A `widgets:` list here would be silently dropped, so that one
        // stops the load; the scalars only warn.
        if (key == "widgets")
        {
            issues.push_back({Issue::Severity::error, key,
                              "a config has either a top-level `widgets:` list or a `windows:` list, "
                              "not both; move these widgets into one of the windows"});
        }
        else
        {
            issues.push_back({Issue::Severity::warning, key,
                              "unknown key, ignored (window keys belong inside an entry of `windows:`)"});
        }
    }

    const YAML::Node windows = root["windows"];
    if (!windows.IsSequence() || windows.size() == 0)
    {
        issues.push_back({Issue::Severity::error, "windows", "expected a non-empty list of windows"});
        return;
    }

    std::map<std::string, std::size_t> names;
    std::map<std::string, std::size_t> displays;
    for (std::size_t i = 0; i < windows.size(); ++i)
    {
        const std::string prefix = "windows[" + std::to_string(i) + "].";
        const YAML::Node window = windows[i];
        if (!window.IsMap())
        {
            issues.push_back({Issue::Severity::error, "windows[" + std::to_string(i) + "]",
                              "expected a mapping"});
            continue;
        }

        validateWindow(window, prefix, issues, ctx);

        // The window name roots every agent selector into it, so two windows with
        // one name make every widget in both ambiguous.
        if (window["name"] && window["name"].IsScalar())
        {
            const std::string name = window["name"].as<std::string>();
            if (!name.empty())
            {
                if (const auto [it, inserted] = names.emplace(name, i); !inserted)
                {
                    issues.push_back({Issue::Severity::error, prefix + "name",
                                      "'" + name + "' is already the name of windows[" +
                                          std::to_string(it->second) + "]"});
                }
            }
        }

        // One window per display. Two on one would stack full-screen on the same
        // panel, and which ends up on top is an accident of construction order.
        std::string display = "primary";
        if (window["display"] && window["display"].IsScalar())
        {
            display = window["display"].as<std::string>();
        }
        if (const auto [it, inserted] = displays.emplace(display, i); !inserted)
        {
            issues.push_back({Issue::Severity::error, prefix + "display",
                              "windows[" + std::to_string(it->second) + "] is already on the '" +
                                  display + "' display" +
                                  (window["display"] ? "" : " (a window with no `display:` is primary)")});
        }
    }
}

}  // namespace

std::vector<Issue> validate_app_config(const YAML::Node& root)
{
    std::vector<Issue> issues;

    if (!root.IsMap())
    {
        issues.push_back({Issue::Severity::error, "", "the top level of a config must be a mapping"});
        return issues;
    }

    ValidationContext ctx;
    if (root["windows"])
    {
        validateWindowList(root, issues, ctx);
    }
    else
    {
        validateWindow(root, "", issues, ctx);
    }
    validateCommands(ctx, issues);

    // Refused outright rather than warned about. Every way a key can be wrong
    // is a silent failure downstream -- '@' makes the topic invisible to every
    // wildcard subscription including discovery, '%' cannot be recovered from
    // an advertisement, and the characters zenoh rejects make the publisher
    // fail to declare and then quietly send nothing. A config that names one
    // does not do what it says, so it does not load.
    for (const pub_sub::TopicKeyIssue& bad : pub_sub::findBadTopicKeys(root))
    {
        issues.push_back({Issue::Severity::error, bad.path,
                          "'" + bad.key + "' is not a usable zenoh key: " + bad.problem});
    }

    return issues;
}


std::optional<dashboard_config_t> load_dashboard_config(const std::string& config_filepath)
{
    // Default config in case of error.
    std::optional<dashboard_config_t> config = std::nullopt;

    try
    {
        const YAML::Node root = YAML::LoadFile(config_filepath);

        // Validate before decoding. The decoder is driven by each struct's
        // fields, so anything the structs do not claim is invisible to it: a
        // mistyped key was dropped in silence and turned up much later as a
        // blank gauge. Report every problem in the file at once, with its path,
        // rather than aborting on the first one yaml-cpp happens to throw at.
        bool fatal = false;
        for (const auto& issue : validate_app_config(root))
        {
            const std::string where = issue.path.empty() ? config_filepath
                                                         : config_filepath + ": " + issue.path;
            if (issue.severity == config_codec::Issue::Severity::error)
            {
                fatal = true;
                SPDLOG_ERROR("{}: {}", where, issue.message);
            }
            else
            {
                SPDLOG_WARN("{}: {}", where, issue.message);
            }
        }

        if (fatal)
        {
            SPDLOG_CRITICAL("Refusing to load '{}': see the errors above.", config_filepath);
            return std::nullopt;
        }

        config = root.as<dashboard_config_t>();
    }
    catch (const YAML::BadFile& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::BadFile : {})", e.what());
    }
    catch (const YAML::ParserException& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::ParserException : {})", e.what());
    }
    catch (const YAML::BadConversion& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::BadConversion : {})", e.what());
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::Exception : {})", e.what());
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to load app config: (std::exception : {})", e.what());
    }

    return config;
}