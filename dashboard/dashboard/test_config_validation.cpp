// What validate_app_config() says about a bad config.
//
// The point of the validator is diagnostics, so the assertions are about the
// *message*, not just the rejection: an error that does not name the field is
// most of the reason a typo used to cost an afternoon. Every case here is one
// the loader previously accepted in silence or rejected without saying where.

#include "dashboard/app_config.h"
#include "dashboard/config_limits.h"
#include "editor/widget_registry.h"

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

// An unparseable colour reaches QColor as "invalid", which paints as
// transparent black -- or, in a stylesheet, makes Qt drop the whole rule and
// leave the window looking untouched. Both are silent.
void testMalformedColoursAreReported()
{
    const auto issues = issuesFor(R"(
background_color: "not-a-color"
widgets:
  - type: static_text
    config:
      color: "0000FF"
)");

    const Issue* window = find(issues, "background_color");
    check(window != nullptr && window->severity == Issue::Severity::error,
          "a malformed window background colour is an error, got:" + dump(issues));

    const Issue* field = find(issues, "widgets[0].config.color");
    check(field != nullptr && field->severity == Issue::Severity::error,
          "a colour missing its '#' is an error");
    if (field)
    {
        check(field->message.find("#RRGGBB") != std::string::npos,
              "the message says what a colour should look like");
    }
}

void testWellFormedColoursAreAccepted()
{
    for (const char* colour : {"#000", "#00FF00", "#00ff00", "#11223344"})
    {
        const std::string yaml = std::string("background_color: \"") + colour + "\"\nwidgets: []\n";
        check(!hasError(issuesFor(yaml)), std::string("'") + colour + "' is accepted");
    }
}

// ---------------------------------------------------------------- range limits
//
// The validator above catches what a file *says*; these catch what it *means*.
// A well-formed number can still be one the widget cannot draw, and each of
// these was a live defect: a divisor of zero, a tick loop that never terminated,
// undefined behaviour in std::clamp, a 0 ms repaint timer.

void testFullScaleIsNeverZero()
{
    MotecCdl3TachometerConfig_t cdl3;
    cdl3.max_rpm = 0;
    check(!validate(cdl3).empty(), "a zero max_rpm is reported");
    check(cdl3.max_rpm > 0, "a zero max_rpm is raised off zero, so nothing divides by it");
}

void testFullScaleIsCapped()
{
    // This value made `rpm += 100` wrap on a uint32_t, so the tick loop never
    // terminated -- and once counted rather than accumulated, it still asked for
    // 43 million ticks.
    MotecCdl3TachometerConfig_t cdl3;
    cdl3.max_rpm = 4294967200u;
    check(!validate(cdl3).empty(), "an absurd max_rpm is reported");
    check(cdl3.max_rpm <= dashboard::limits::kMaxRpmCeiling,
          "an absurd max_rpm is capped to something drawable");
}

void testRedlineCannotExceedFullScale()
{
    Mercedes190ETachometerConfig_t tach;
    tach.max_rpm = 7000;
    tach.redline_rpm = 9000;
    check(!validate(tach).empty(), "a redline above max_rpm is reported");
    check(tach.redline_rpm <= tach.max_rpm,
          "a redline above max_rpm is pulled back, so the red zone is not drawn backwards");
}

void testInvertedRangesAreOrdered()
{
    // std::clamp's precondition is !(max < min); the cluster gauge clamped four
    // readings against unvalidated pairs.
    Mercedes190EClusterGaugeConfig_t cluster;
    cluster.fuel_gauge.min_value = 100.0f;
    cluster.fuel_gauge.max_value = 0.0f;
    check(!validate(cluster).empty(), "an inverted range is reported");
    check(cluster.fuel_gauge.min_value < cluster.fuel_gauge.max_value,
          "an inverted range comes back the right way round");

    // A zero-width range makes the value-to-position division degenerate.
    Mercedes190EClusterGaugeConfig_t flat;
    flat.left_gauge.min_value = 50.0f;
    flat.left_gauge.max_value = 50.0f;
    check(!validate(flat).empty(), "a zero-width range is reported");
    check(flat.left_gauge.min_value < flat.left_gauge.max_value, "a zero-width range is widened");
}

void testEconomyRedStartStaysOnTheBand()
{
    // red_start_fraction is the parameter the economy band's red section is
    // walked from. Below 0 it starts off the economical end of the band; above 1
    // the sub-band runs backwards and paints past the uneconomical end.
    Mercedes190EClusterGaugeConfig_t below;
    below.economy_sweep.red_start_fraction = -0.5f;
    check(!validate(below).empty(), "a negative red_start_fraction is reported");
    check(below.economy_sweep.red_start_fraction >= 0.0f,
          "a negative red_start_fraction is pulled onto the band");

    Mercedes190EClusterGaugeConfig_t above;
    above.economy_sweep.red_start_fraction = 4.0f;
    check(!validate(above).empty(), "a red_start_fraction past the end is reported");
    check(above.economy_sweep.red_start_fraction <= 1.0f,
          "a red_start_fraction past the end is pulled back onto the band");

    // The default has to survive validation untouched, or every stock cluster
    // logs a spurious adjustment at load.
    Mercedes190EClusterGaugeConfig_t stock;
    const float before = stock.economy_sweep.red_start_fraction;
    (void)validate(stock);
    check(stock.economy_sweep.red_start_fraction == before,
          "the default red_start_fraction is left alone");
}

void testUpdateRateCannotBecomeAZeroMillisecondTimer()
{
    // update_rate feeds `1000 / update_rate` as a millisecond interval, so 2000
    // became start(0) -- a repaint on every pass of the event loop.
    SparklineConfig_t spark;
    spark.update_rate = 2000;
    check(!validate(spark).empty(), "an absurd update_rate is reported");
    check(spark.update_rate > 0 && 1000 / spark.update_rate > 0,
          "the clamped update_rate still yields a non-zero timer interval");

    SparklineConfig_t zero;
    zero.update_rate = 0;
    (void)validate(zero);
    check(zero.update_rate > 0, "a zero update_rate is raised off zero");
}

void testOverlongListsAreCapped()
{
    Mercedes190ESpeedometerConfig_t speedo;
    speedo.shift_box_markers.assign(5000, 42);
    check(!validate(speedo).empty(), "an overlong marker list is reported");
    check(speedo.shift_box_markers.size() <= dashboard::limits::kMaxMarkers,
          "an overlong marker list is truncated, so paint stays bounded");
}

void testOdometerCannotOutrunItsDigits()
{
    // The odometer renders six digits. A larger value silently displayed the
    // wrong ones: the zenoh setter clamped, the config path did not.
    Mercedes190ESpeedometerConfig_t speedo;
    speedo.odometer_value = 12345678;
    check(!validate(speedo).empty(), "an out-of-range odometer value is reported");
    check(speedo.odometer_value <= 999999, "an out-of-range odometer value is clamped to six digits");
}

void testAReasonableConfigIsLeftAlone()
{
    MotecCdl3TachometerConfig_t cdl3;
    cdl3.max_rpm = 8000;
    check(validate(cdl3).empty(), "a sensible config produces no adjustments");
    check(cdl3.max_rpm == 8000, "a sensible config is not modified");

    SparklineConfig_t spark;
    spark.update_rate = 30;
    spark.min_value = 0.0;
    spark.max_value = 100.0;
    check(validate(spark).empty(), "a sensible sparkline config produces no adjustments");
    check(spark.update_rate == 30 && spark.max_value == 100.0,
          "a sensible sparkline config is not modified");
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
    testMalformedColoursAreReported();
    testWellFormedColoursAreAccepted();

    testFullScaleIsNeverZero();
    testFullScaleIsCapped();
    testRedlineCannotExceedFullScale();
    testInvertedRangesAreOrdered();
    testEconomyRedStartStaysOnTheBand();
    testUpdateRateCannotBecomeAZeroMillisecondTimer();
    testOverlongListsAreCapped();
    testOdometerCannotOutrunItsDigits();
    testAReasonableConfigIsLeftAlone();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
