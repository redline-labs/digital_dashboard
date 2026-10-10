// Typed parameter reading. What matters is that a WRONG type is an error naming
// the parameter, never mistaken for an absent one -- the bug these replaced --
// and that an integer is range-checked for the type it lands in.

#include "agent_control/params.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using agent_control::ErrorCode;
using agent_control::json;
using agent_control::optionalParam;
using agent_control::requireParam;

bool isBadParamsNaming(const agent_control::AgentError& error, const std::string& name)
{
    return error.code == ErrorCode::kBadParams &&
           error.message.find("'" + name + "'") != std::string::npos;
}

}  // namespace

int main()
{
    const json params = {{"id", "plot"},       {"count", 3},        {"negative", -1},
                         {"big", 70000},       {"flag", true},      {"ratio", 0.5},
                         {"whole", 2},         {"nothing", nullptr}, {"config", {{"a", 1}}},
                         {"list", json::array()}};

    // Present and right.
    {
        const auto id = requireParam<std::string>(params, "id");
        check(id && *id == "plot", "a string reads");
        const auto count = requireParam<std::size_t>(params, "count");
        check(count && *count == 3, "a size_t reads");
        const auto flag = requireParam<bool>(params, "flag");
        check(flag && *flag, "a bool reads");
        const auto ratio = requireParam<double>(params, "ratio");
        check(ratio && *ratio == 0.5, "a double reads");
        const auto whole = requireParam<double>(params, "whole");
        check(whole && *whole == 2.0, "an integer is a number");
        const auto negative = requireParam<int>(params, "negative");
        check(negative && *negative == -1, "a negative int reads");
        const auto config = requireParam<json>(params, "config");
        check(config && config->value("a", 0) == 1, "an object reads");
    }

    // Absent and null are the same, and neither is an error when optional.
    {
        const auto absent = optionalParam<std::string>(params, "missing");
        check(absent && !absent->has_value(), "an absent optional is nullopt");
        const auto null = optionalParam<std::string>(params, "nothing");
        check(null && !null->has_value(), "a null optional is nullopt");
        const auto required = requireParam<std::string>(params, "nothing");
        check(!required && isBadParamsNaming(required.error(), "nothing"),
              "a null required parameter is an error naming it");
        const auto missing = requireParam<int>(params, "missing");
        check(!missing && isBadParamsNaming(missing.error(), "missing"),
              "an absent required parameter is an error naming it");
    }

    // The wrong type is an error, required or not -- never silently absent.
    {
        const auto id = optionalParam<std::string>(params, "count");
        check(!id && isBadParamsNaming(id.error(), "count"),
              "a number where a string belongs is an error, not nullopt");
        const auto flag = optionalParam<bool>(params, "id");
        check(!flag && isBadParamsNaming(flag.error(), "id"), "a string is not a bool");
        const auto count = optionalParam<int>(params, "ratio");
        check(!count && isBadParamsNaming(count.error(), "ratio"), "0.5 is not an integer");
        const auto truthy = optionalParam<int>(params, "flag");
        check(!truthy, "true is not an integer");
        const auto config = optionalParam<json>(params, "list");
        check(!config && isBadParamsNaming(config.error(), "list"), "an array is not an object");
    }

    // Integers are checked against the type they land in.
    {
        const auto negative = optionalParam<std::size_t>(params, "negative");
        check(!negative && isBadParamsNaming(negative.error(), "negative"),
              "-1 is not a size_t, rather than a very large one");
        const auto big = optionalParam<std::uint16_t>(params, "big");
        check(!big, "70000 does not fit a uint16_t, rather than wrapping to 4464");
        const auto fits = optionalParam<std::int64_t>(params, "big");
        check(fits && fits->value_or(0) == 70000, "70000 fits an int64");
    }

    // Params that are not an object at all name nothing.
    {
        const auto from_null = optionalParam<std::string>(json(nullptr), "id");
        check(from_null && !from_null->has_value(), "null params have no parameters");
        const auto from_array = requireParam<std::string>(json::array(), "id");
        check(!from_array && isBadParamsNaming(from_array.error(), "id"),
              "an array of params is missing the parameter");
    }

    // Several reads, one refusal: the first failure in argument order.
    {
        const auto id = requireParam<std::string>(params, "id");
        const auto wrong = optionalParam<bool>(params, "count");
        const auto absent = requireParam<int>(params, "missing");
        const auto error = agent_control::firstError(id, wrong, absent);
        check(error && isBadParamsNaming(*error, "count"), "firstError is the first failure");
        check(!agent_control::firstError(id), "firstError is empty when all succeeded");
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
