// Every shipped dashboard layout must be clean: no validator issue -- warnings
// included -- and no value a widget's validate() would clamp.
//
// A warning here is never cosmetic. "unknown key, ignored" is exactly what a
// renamed config field leaves behind in a layout nobody rewrote, and the
// binding it named is silently gone. Loading is not the bar; loading with
// nothing to say is.

#include "dashboard/app_config.h"
#include "config_codec/config_apply_limits.h"
#include "dashboard/widget_registry.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <variant>
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

// What validate() would change about each widget, recursing into pages.
void collectClampNotes(const std::vector<widget_config_t>& widgets, const std::string& path,
                       std::vector<std::string>& notes)
{
    for (std::size_t i = 0; i < widgets.size(); ++i)
    {
        const std::string here = path + "[" + std::to_string(i) + "]";
        std::visit([&](const auto& cfg)
        {
            auto copy = cfg;
            for (const std::string& note : config_codec::applyLimits(copy))
            {
                notes.push_back(here + ": " + note);
            }
        }, widgets[i].config);

        for (std::size_t p = 0; p < widgets[i].pages.size(); ++p)
        {
            collectClampNotes(widgets[i].pages[p].widgets,
                              here + ".pages[" + std::to_string(p) + "].widgets", notes);
        }
    }
}

void checkFile(const std::filesystem::path& file)
{
    const std::string name = file.filename().string();

    YAML::Node root;
    try
    {
        root = YAML::LoadFile(file.string());
    }
    catch (const std::exception& e)
    {
        check(false, name + " parses: " + e.what());
        return;
    }

    std::string issues;
    for (const auto& issue : validate_app_config(root))
    {
        issues += "\n    " + issue.path + ": " + issue.message;
    }
    check(issues.empty(), name + " validates with no issues:" + issues);

    const auto config = load_dashboard_config(file.string());
    check(config.has_value(), name + " loads");
    if (!config)
    {
        return;
    }

    std::vector<std::string> notes;
    for (std::size_t w = 0; w < config->windows.size(); ++w)
    {
        collectClampNotes(config->windows[w].widgets,
                          "windows[" + std::to_string(w) + "].widgets", notes);
    }
    std::string joined;
    for (const auto& note : notes)
    {
        joined += "\n    " + note;
    }
    check(notes.empty(), name + " has nothing validate() would clamp:" + joined);
}

}  // namespace

int main()
{
    const std::filesystem::path dir(DASHBOARD_CONFIG_DIR);
    check(std::filesystem::is_directory(dir), "the shipped layout directory exists");

    int found = 0;
    if (std::filesystem::is_directory(dir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() == ".yaml")
            {
                ++found;
                checkFile(entry.path());
            }
        }
    }
    check(found > 0, "there is at least one shipped layout to check");

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
