// String leaves -- colours today, zenoh keys next -- through every codec: YAML
// both ways, the validator, the JSON patch path and describe().
//
// The validator and the JSON path are the two that matter. A leaf whose rules
// are skipped on either one is accepted as a plain string and fails later,
// silently: an invalid colour paints transparent, a bad key subscribes to
// nothing.

#include "config_codec/config_json.h"
#include "config_codec/config_validation.h"
#include "config_codec/config_yaml.h"
#include "helpers/color.h"

#include <cstdio>
#include <string>
#include <vector>

REFLECT_STRUCT(Swatch,
    (helpers::Color, fill, "#112233")
)

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

std::vector<config_codec::Issue> validateYaml(const std::string& text)
{
    std::vector<config_codec::Issue> issues;
    config_codec::detail::validateStruct<Swatch>(YAML::Load(text), "", issues);
    return issues;
}

void testYamlRoundTrip()
{
    Swatch swatch;
    swatch.fill = helpers::Color("#ABCDEF");
    const YAML::Node node = YAML::convert<Swatch>::encode(swatch);
    check(node["fill"].IsScalar() && node["fill"].as<std::string>() == "#ABCDEF",
          "a leaf is written as a plain scalar");
    check(YAML::Load("fill: \"#010203\"").as<Swatch>().fill == helpers::Color("#010203"),
          "and read back from one");
}

void testValidatorAppliesTheLeafRules()
{
    check(validateYaml("fill: \"#00ff00\"").empty(), "a good colour has nothing to report");

    const auto bad = validateYaml("fill: \"#GG0000\"");
    check(bad.size() == 1 && bad[0].severity == config_codec::Issue::Severity::error &&
              bad[0].path == "fill" &&
              bad[0].message == "'#GG0000' is not a colour; expected #RGB, #RRGGBB or #RRGGBBAA",
          "a bad colour is an error naming the field and the value");

    const auto nested = validateYaml("fill: [1, 2]");
    check(nested.size() == 1 && nested[0].message == "expected a color string",
          "a collection where a leaf belongs is refused by type");
}

void testJsonPatchAppliesTheLeafRules()
{
    Swatch swatch;
    std::vector<std::string> errors;
    config_codec::applyJson(config_codec::json{{"fill", "#123"}}, swatch, "", errors);
    check(errors.empty() && swatch.fill == helpers::Color("#123"), "a good patch applies");

    errors.clear();
    config_codec::applyJson(config_codec::json{{"fill", "red"}}, swatch, "", errors);
    check(errors.size() == 1 && swatch.fill == helpers::Color("#123"),
          "a bad patch is refused and leaves the field alone");
}

void testDescribeReportsTheLeafType()
{
    const auto described = config_codec::describeType<Swatch>();
    check(described["fields"]["fill"]["type"] == "color", "describe names the leaf type");
    check(described["fields"]["fill"]["default"] == "#112233", "and its default as a string");
}

}  // namespace

int main()
{
    testYamlRoundTrip();
    testValidatorAppliesTheLeafRules();
    testJsonPatchAppliesTheLeafRules();
    testDescribeReportsTheLeafType();

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
