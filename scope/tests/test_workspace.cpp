// SPDX-License-Identifier: GPL-3.0-or-later
//
// The workspace codec: YAML in, YAML out, and what happens to a file that is
// wrong in each of the ways a hand-edited one tends to be.
//
// The byte-stability check over every shipped workspace is the important one,
// and it is borrowed from editor_test_roundtrip. A codec that round-trips
// *semantically* but reorders keys or drops a default turns every save into a
// diff, which makes a workspace impossible to keep in version control -- and
// that is exactly what someone will want to do with one.
//
// No Qt widgets and no zenoh, so this is a plain unit test.

#include "scope/workspace.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::filesystem::path tempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

// ------------------------------------------------------- the shipped corpus

void testShippedWorkspacesLoad()
{
    const std::filesystem::path dir(SCOPE_CONFIG_DIR);
    if (!std::filesystem::is_directory(dir))
    {
        expect(false, "the shipped workspace directory exists");
        return;
    }

    int found = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir))
    {
        if (file.path().extension() != ".yaml")
        {
            continue;
        }
        ++found;

        const auto workspace = scope::load_workspace(file.path().string());
        expect(workspace.has_value(),
               "shipped workspace " + file.path().filename().string() + " loads");
        if (workspace)
        {
            expect(!workspace->panels.empty(),
                   "shipped workspace " + file.path().filename().string() + " has panels");
        }
    }

    expect(found > 0, "there is at least one shipped workspace to check");
}

void testShippedWorkspacesRoundTripByteForByte()
{
    // Load, save, load, save: the second save must equal the first. Comparing
    // against the file on disk would fail on formatting the emitter never
    // produces (comments, key order, quoting), which is not what this is
    // guarding -- what matters is that saving an unmodified workspace twice
    // does not keep changing it.
    const std::filesystem::path dir(SCOPE_CONFIG_DIR);
    if (!std::filesystem::is_directory(dir))
    {
        return;
    }

    for (const auto& file : std::filesystem::directory_iterator(dir))
    {
        if (file.path().extension() != ".yaml")
        {
            continue;
        }

        const auto first = scope::load_workspace(file.path().string());
        if (!first)
        {
            continue;  // Reported by the test above.
        }

        const std::filesystem::path once = tempPath("scope_roundtrip_1.yaml");
        const std::filesystem::path twice = tempPath("scope_roundtrip_2.yaml");

        expect(scope::save_workspace(*first, once.string()), "a loaded workspace saves");

        const auto second = scope::load_workspace(once.string());
        expect(second.has_value(), "a saved workspace loads back");
        if (!second)
        {
            continue;
        }

        expect(scope::save_workspace(*second, twice.string()), "a reloaded workspace saves");
        expect(readFile(once) == readFile(twice),
               "saving " + file.path().filename().string() +
                   " twice produces byte-identical output");

        std::filesystem::remove(once);
        std::filesystem::remove(twice);
    }
}

// --------------------------------------------------------------- round trip

scope::scope_workspace_t sampleWorkspace()
{
    scope::scope_workspace_t workspace;
    workspace.name = "Test";
    workspace.history_seconds = 120.0;
    workspace.window_seconds = 15.0;
    workspace.render_rate_hz = 24;
    workspace.max_capture_bytes = 268435456;
    workspace.max_capture_seconds = 600.0;

    TimeSeriesPanelConfig_t plot;
    plot.title = "Engine";
    plot.autoscale_y = false;
    plot.y_min = -5.0;
    plot.y_max = 7000.0;

    signal_binding_t binding;
    binding.zenoh_key = "vehicle/engine/rpm";
    binding.schema_type = pub_sub::schema_type_t::EngineRpm;
    binding.value_expression = "rpm / 1000.0";
    binding.label = "krpm";
    binding.units = "krpm";
    binding.right_axis = true;
    plot.traces.push_back(binding);

    scope::panel_entry_t entry;
    entry.id = "engine";
    entry.type = scope::panel_type_t::time_series;
    entry.config = plot;
    workspace.panels.push_back(entry);

    workspace.dock_state = "AAAA";
    return workspace;
}

void testEveryFieldSurvivesARoundTrip()
{
    const scope::scope_workspace_t original = sampleWorkspace();
    const std::filesystem::path path = tempPath("scope_fields.yaml");

    expect(scope::save_workspace(original, path.string()), "a workspace saves");

    const auto loaded = scope::load_workspace(path.string());
    expect(loaded.has_value(), "a workspace loads");
    if (!loaded)
    {
        return;
    }

    expect(loaded->name == original.name, "the name survives");
    expect(loaded->history_seconds == original.history_seconds, "history_seconds survives");
    expect(loaded->window_seconds == original.window_seconds, "window_seconds survives");
    expect(loaded->render_rate_hz == original.render_rate_hz, "render_rate_hz survives");
    expect(loaded->max_capture_bytes == original.max_capture_bytes,
           "max_capture_bytes survives");
    expect(loaded->max_capture_seconds == original.max_capture_seconds,
           "max_capture_seconds survives");
    expect(loaded->dock_state == original.dock_state, "the dock state blob survives verbatim");
    expect(loaded->panels.size() == 1, "the panel survives");

    if (loaded->panels.size() != 1)
    {
        return;
    }

    expect(loaded->panels[0].id == "engine", "the panel id survives");
    expect(loaded->panels[0].type == scope::panel_type_t::time_series, "the panel type survives");

    const auto* plot = std::get_if<TimeSeriesPanelConfig_t>(&loaded->panels[0].config);
    expect(plot != nullptr, "the config decodes to the right variant alternative");
    if (plot == nullptr)
    {
        return;
    }

    expect(plot->title == "Engine", "the panel title survives");
    expect(!plot->autoscale_y, "a false boolean survives rather than reverting to its default");
    expect(plot->y_min == -5.0, "a negative double survives");
    expect(plot->traces.size() == 1, "the trace survives");

    if (plot->traces.empty())
    {
        return;
    }

    // The binding triple is the part that matters most: get any of it wrong and
    // the plot silently shows nothing, or shows the wrong field's bytes.
    expect(plot->traces[0].zenoh_key == "vehicle/engine/rpm", "the zenoh key survives");
    expect(plot->traces[0].schema_type == pub_sub::schema_type_t::EngineRpm,
           "the schema type survives, by name rather than by ordinal");
    expect(plot->traces[0].value_expression == "rpm / 1000.0", "the expression survives verbatim");
    expect(plot->traces[0].units == "krpm", "the units survive");
    expect(plot->traces[0].right_axis, "the axis assignment survives");

    std::filesystem::remove(path);
}

void testEqualityFollowsTheFields()
{
    const scope::scope_workspace_t a = sampleWorkspace();
    scope::scope_workspace_t b = sampleWorkspace();
    expect(a == b, "two identically-built workspaces compare equal");

    b.window_seconds = 99.0;
    expect(!(a == b), "a changed field makes them differ");
}

// ----------------------------------------------------------- malformed input

void testAMissingFileIsReportedNotThrown()
{
    expect(!scope::load_workspace("/nonexistent/nowhere.yaml").has_value(),
           "a missing file returns nullopt rather than throwing");
}

void testUnparseableYamlIsReportedNotThrown()
{
    const std::filesystem::path path = tempPath("scope_bad.yaml");
    {
        std::ofstream out(path);
        out << "panels: [ this is not: valid: yaml\n";
    }
    expect(!scope::load_workspace(path.string()).has_value(),
           "a file that is not YAML at all returns nullopt rather than throwing");
    std::filesystem::remove(path);
}

void testATopLevelSequenceIsRejected()
{
    const std::filesystem::path path = tempPath("scope_seq.yaml");
    {
        std::ofstream out(path);
        out << "- one\n- two\n";
    }
    expect(!scope::load_workspace(path.string()).has_value(),
           "a workspace that is not a mapping is rejected with an error, not decoded as empty");
    std::filesystem::remove(path);
}

void testPanelsMustBeASequence()
{
    const std::filesystem::path path = tempPath("scope_panels_map.yaml");
    {
        std::ofstream out(path);
        out << "name: x\npanels:\n  not: a sequence\n";
    }
    expect(!scope::load_workspace(path.string()).has_value(),
           "'panels' as a mapping is an error rather than a silent empty list");
    std::filesystem::remove(path);
}

void testAnUnknownPanelTypeIsSkippedNotFatal()
{
    // A workspace written by a newer build may name a panel type this one has
    // never heard of. Refusing the whole file over that would make an upgrade
    // one-way, so it is a warning and the panel is skipped.
    const std::filesystem::path path = tempPath("scope_unknown_type.yaml");
    {
        std::ofstream out(path);
        out << "name: x\npanels:\n"
               "  - id: a\n    type: time_series\n"
               "  - id: b\n    type: holographic_projection\n";
    }

    const auto loaded = scope::load_workspace(path.string());
    expect(loaded.has_value(), "an unknown panel type does not stop the file loading");
    if (loaded)
    {
        expect(loaded->panels.size() == 2, "both entries decode");
        expect(loaded->panels[0].type == scope::panel_type_t::time_series,
               "the known panel keeps its type");
        expect(loaded->panels[1].type == scope::panel_type_t::unknown,
               "the unknown panel is marked unknown so the loader can skip it");
    }
    std::filesystem::remove(path);
}

void testAMissingConfigBlockDefaultConstructs()
{
    // This exact case was a bug in the dashboard's decoder: it read
    // node["config"] unconditionally, yaml-cpp throws on an undefined node, and
    // the exception failed the whole file -- for a config its own validator had
    // just passed.
    const std::filesystem::path path = tempPath("scope_no_config.yaml");
    {
        std::ofstream out(path);
        out << "name: x\npanels:\n  - id: a\n    type: time_series\n";
    }

    const auto loaded = scope::load_workspace(path.string());
    expect(loaded.has_value(), "a panel with no 'config:' block loads");
    if (loaded && loaded->panels.size() == 1)
    {
        const auto* plot = std::get_if<TimeSeriesPanelConfig_t>(&loaded->panels[0].config);
        expect(plot != nullptr,
               "a missing config block default-constructs rather than leaving monostate");
        if (plot != nullptr)
        {
            expect(plot->traces.empty(), "the default config has no traces");
        }
    }
    std::filesystem::remove(path);
}

void testAPanelWithNoIdLoadsWithAWarning()
{
    const std::filesystem::path path = tempPath("scope_no_id.yaml");
    {
        std::ofstream out(path);
        out << "name: x\npanels:\n  - type: time_series\n";
    }

    const auto loaded = scope::load_workspace(path.string());
    expect(loaded.has_value(), "a panel with no id still loads; the window assigns one");
    if (loaded)
    {
        const std::vector<config_codec::Issue> issues =
            scope::validate_workspace(YAML::LoadFile(path.string()));
        const bool warned = std::any_of(issues.begin(), issues.end(),
                                        [](const config_codec::Issue& issue) {
                                            return issue.severity ==
                                                   config_codec::Issue::Severity::warning;
                                        });
        expect(warned, "a panel with no id is warned about, because the dock state cannot match it");
    }
    std::filesystem::remove(path);
}

void testAnEmptyWorkspaceIsValid()
{
    const std::filesystem::path path = tempPath("scope_empty.yaml");
    {
        std::ofstream out(path);
        out << "name: nothing\n";
    }
    const auto loaded = scope::load_workspace(path.string());
    expect(loaded.has_value(), "a workspace with no panels is valid, not an error");
    if (loaded)
    {
        expect(loaded->panels.empty(), "it has no panels");
        expect(loaded->window_seconds == 30.0, "unspecified fields take their declared defaults");
    }
    std::filesystem::remove(path);
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testShippedWorkspacesLoad();
    testShippedWorkspacesRoundTripByteForByte();

    testEveryFieldSurvivesARoundTrip();
    testEqualityFollowsTheFields();

    testAMissingFileIsReportedNotThrown();
    testUnparseableYamlIsReportedNotThrown();
    testATopLevelSequenceIsRejected();
    testPanelsMustBeASequence();
    testAnUnknownPanelTypeIsSkippedNotFatal();
    testAMissingConfigBlockDefaultConstructs();
    testAPanelWithNoIdLoadsWithAWarning();
    testAnEmptyWorkspaceIsValid();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
