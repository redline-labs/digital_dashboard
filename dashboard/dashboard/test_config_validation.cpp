// What validate_app_config() says about a bad config.
//
// The point of the validator is diagnostics, so the assertions are about the
// *message*, not just the rejection: an error that does not name the field is
// most of the reason a typo used to cost an afternoon. Every case here is one
// the loader previously accepted in silence or rejected without saying where.

#include "dashboard/app_config.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <string>
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

using dashboard::config::Issue;

std::vector<Issue> issuesFor(const std::string& yaml)
{
    return validate_app_config(YAML::Load(yaml));
}

// Finds an issue by the path it points at.
const Issue* find(const std::vector<Issue>& issues, const std::string& path)
{
    for (const auto& issue : issues)
    {
        if (issue.path == path)
        {
            return &issue;
        }
    }
    return nullptr;
}

bool hasError(const std::vector<Issue>& issues)
{
    for (const auto& issue : issues)
    {
        if (issue.severity == Issue::Severity::error)
        {
            return true;
        }
    }
    return false;
}

std::string dump(const std::vector<Issue>& issues)
{
    std::string out;
    for (const auto& issue : issues)
    {
        out += "\n    [" + std::string(issue.severity == Issue::Severity::error ? "error" : "warn") +
               "] " + issue.path + ": " + issue.message;
    }
    return out.empty() ? " (none)" : out;
}

const char* kValidWidget = R"(
name: "ok"
width: 600
height: 300
widgets:
  - type: motec_cdl3_tachometer
    x: 0
    y: 0
    width: 100
    height: 100
    config:
      max_rpm: 6000
      zenoh_key: "vehicle/engine/rpm"
      schema_type: "EngineRpm"
      rpm_expression: "rpm"
)";

void testAValidConfigIsSilent()
{
    const auto issues = issuesFor(kValidWidget);
    check(issues.empty(), "a valid config produces no issues, got:" + dump(issues));
}

// The headline case: a mistyped key was dropped without a word, and showed up
// much later as a widget subscribed to "".
void testUnknownKeyIsReportedWithItsPath()
{
    const auto issues = issuesFor(R"(
name: "typo"
widgets:
  - type: motec_cdl3_tachometer
    config:
      zenoh_kye: "vehicle/engine/rpm"
)");

    const Issue* issue = find(issues, "widgets[0].config.zenoh_kye");
    check(issue != nullptr, "a mistyped config key is reported at its full path, got:" + dump(issues));
    if (issue)
    {
        check(issue->severity == Issue::Severity::warning, "an unknown key is a warning, not an error");
        check(issue->message.find("zenoh_key") != std::string::npos,
              "the message lists the keys that do exist, so the typo is obvious");
    }
    check(!hasError(issues), "an unknown key alone does not stop the load");
}

void testUnknownTopLevelAndWidgetKeys()
{
    const auto issues = issuesFor(R"(
name: "typo"
widht: 600
widgets:
  - type: static_text
    xx: 5
    config: {}
)");

    check(find(issues, "widht") != nullptr, "an unknown window key is reported, got:" + dump(issues));
    check(find(issues, "widgets[0].xx") != nullptr, "an unknown placement key is reported");
}

void testUnknownWidgetTypeNamesTheAlternatives()
{
    const auto issues = issuesFor(R"(
widgets:
  - type: no_such_widget
)");

    const Issue* issue = find(issues, "widgets[0].type");
    check(issue != nullptr, "an unknown widget type is reported, got:" + dump(issues));
    if (issue)
    {
        check(issue->severity == Issue::Severity::error, "an unknown widget type stops the load");
        check(issue->message.find("no_such_widget") != std::string::npos,
              "the message quotes the offending type");
        check(issue->message.find("static_text") != std::string::npos,
              "the message lists real widget types");

        // `unknown` is an internal state. Offering it as a suggestion sends the
        // reader off to write `type: unknown`, which cannot work. Look only at
        // the suggestion list -- the word also appears in the message's own
        // prose ("unknown widget type ...").
        const std::size_t list_start = issue->message.find("expected one of: ");
        check(list_start != std::string::npos, "the message has a suggestion list");
        if (list_start != std::string::npos)
        {
            const std::string list = issue->message.substr(list_start);
            check(list.find("unknown") == std::string::npos,
                  "the suggestion list does not offer 'unknown' as a choice");
        }
    }
}

void testMissingTypeIsAnError()
{
    const auto issues = issuesFor(R"(
widgets:
  - x: 5
    y: 5
)");
    const Issue* issue = find(issues, "widgets[0].type");
    check(issue != nullptr && issue->severity == Issue::Severity::error,
          "a widget with no type is an error, got:" + dump(issues));
}

void testBadEnumValueNamesTheAlternatives()
{
    const auto issues = issuesFor(R"(
widgets:
  - type: motec_cdl3_tachometer
    config:
      schema_type: "EngineRpmm"
)");

    const Issue* issue = find(issues, "widgets[0].config.schema_type");
    check(issue != nullptr, "a bad enum value is reported at its path, got:" + dump(issues));
    if (issue)
    {
        check(issue->severity == Issue::Severity::error, "a bad enum value stops the load");
        check(issue->message.find("EngineRpmm") != std::string::npos,
              "the message quotes the offending value");
        check(issue->message.find("EngineRpm") != std::string::npos,
              "the message lists valid schema names");

        // The schema enum has 70-odd entries; dumping them all buries the
        // problem it is trying to explain.
        check(issue->message.find("more)") != std::string::npos,
              "a long list of alternatives is truncated");
    }
}

void testWrongScalarTypeIsAnError()
{
    const auto issues = issuesFor(R"(
widgets:
  - type: motec_cdl3_tachometer
    config:
      max_rpm: "not a number"
)");
    const Issue* issue = find(issues, "widgets[0].config.max_rpm");
    check(issue != nullptr && issue->severity == Issue::Severity::error,
          "a non-numeric value in a numeric field is an error, got:" + dump(issues));
}

void testWrongShapeIsAnError()
{
    check(hasError(issuesFor("widgets: 5")), "a scalar where the widget list belongs is an error");
    check(hasError(issuesFor("[1, 2, 3]")), "a sequence at the top level is an error");

    const auto issues = issuesFor(R"(
widgets:
  - type: static_text
    config: "should be a map"
)");
    check(hasError(issues), "a scalar where a config block belongs is an error, got:" + dump(issues));
}

// Every problem in the file at once. The loader used to abort on whichever one
// yaml-cpp threw at first, so fixing a config was one round trip per mistake.
void testAllProblemsAreReportedTogether()
{
    const auto issues = issuesFor(R"(
widgets:
  - type: motec_cdl3_tachometer
    config:
      max_rpm: "not a number"
      zenoh_kye: "vehicle/engine/rpm"
      schema_type: "EngineRpmm"
)");

    check(find(issues, "widgets[0].config.max_rpm") != nullptr &&
              find(issues, "widgets[0].config.zenoh_kye") != nullptr &&
              find(issues, "widgets[0].config.schema_type") != nullptr,
          "three problems in one widget are all reported, got:" + dump(issues));
}

// A widget with no config block is legal -- it takes defaults -- but saying so
// is worth a line, because it is rarely deliberate.
void testMissingConfigIsAWarningNotAnError()
{
    const auto issues = issuesFor(R"(
widgets:
  - type: static_text
)");
    const Issue* issue = find(issues, "widgets[0].config");
    check(issue != nullptr && issue->severity == Issue::Severity::warning,
          "a widget with no config block warns but loads, got:" + dump(issues));
}

// The shipped configs are the ones people copy from; they must be clean.
void testNestedStructsAreWalked()
{
    // The cluster gauge is the only config with nested reflected structs, so it
    // is the one that proves the walk recurses rather than stopping at depth 1.
    const auto issues = issuesFor(R"(
widgets:
  - type: mercedes_190e_cluster_gauge
    config:
      fuel_gauge:
        min_value: 0.0
        max_valu: 100.0
)");
    const Issue* issue = find(issues, "widgets[0].config.fuel_gauge.max_valu");
    check(issue != nullptr, "a typo inside a nested struct is reported at its full path, got:" + dump(issues));
}

}  // namespace

int main()
{
    testAValidConfigIsSilent();
    testUnknownKeyIsReportedWithItsPath();
    testUnknownTopLevelAndWidgetKeys();
    testUnknownWidgetTypeNamesTheAlternatives();
    testMissingTypeIsAnError();
    testBadEnumValueNamesTheAlternatives();
    testWrongScalarTypeIsAnError();
    testWrongShapeIsAnError();
    testAllProblemsAreReportedTogether();
    testMissingConfigIsAWarningNotAnError();
    testNestedStructsAreWalked();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
