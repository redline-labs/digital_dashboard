#ifndef AGENT_CONTROL_PARAMS_H_
#define AGENT_CONTROL_PARAMS_H_

#include "agent_control/error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace agent_control
{

// Reading one parameter of a method call, typed.
//
// A handler that wrote `if (p != params.end() && p->is_string())` treated a
// parameter of the wrong type exactly like an absent one: `"id": 7` added a
// panel with a generated id, `"panel": 2` meant every panel. The caller asked
// for something specific and was quietly given something else. These answer a
// wrong type with kBadParams naming the parameter instead.
//
// Absent and null are the same thing: an MCP client fills unset optional
// arguments with null. An integer is checked against the target type's range,
// so -1 is not a large size_t.

namespace detail
{

template <typename T>
inline constexpr bool kIsParamInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>;

template <typename T>
std::string_view expectedKind()
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        return "a string";
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return "a boolean";
    }
    else if constexpr (kIsParamInteger<T> && std::is_unsigned_v<T>)
    {
        return "a non-negative integer";
    }
    else if constexpr (kIsParamInteger<T>)
    {
        return "an integer";
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        return "a number";
    }
    else
    {
        static_assert(std::is_same_v<T, json>, "agent_control params: unsupported parameter type");
        return "an object";
    }
}

template <typename T>
std::optional<T> convert(const json& value)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        return value.is_string() ? std::optional<T>(value.get<std::string>()) : std::nullopt;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return value.is_boolean() ? std::optional<T>(value.get<bool>()) : std::nullopt;
    }
    else if constexpr (kIsParamInteger<T>)
    {
        // json keeps a non-negative literal as unsigned and a negative one as
        // signed; read each in its own width, then ask whether it fits.
        if (value.is_number_unsigned())
        {
            const auto n = value.get<std::uint64_t>();
            return std::in_range<T>(n) ? std::optional<T>(static_cast<T>(n)) : std::nullopt;
        }
        if (value.is_number_integer())
        {
            const auto n = value.get<std::int64_t>();
            return std::in_range<T>(n) ? std::optional<T>(static_cast<T>(n)) : std::nullopt;
        }
        return std::nullopt;
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        return value.is_number() ? std::optional<T>(value.get<T>()) : std::nullopt;
    }
    else
    {
        return value.is_object() ? std::optional<T>(value) : std::nullopt;
    }
}

}  // namespace detail

// The parameter, or std::nullopt when it is absent or null; kBadParams when it
// is present with the wrong type. T is std::string, bool, an integer type, a
// floating-point type, or json (which must be an object).
template <typename T>
Result<std::optional<T>> optionalParam(const json& params, std::string_view name)
{
    if (!params.is_object())
    {
        return std::nullopt;
    }
    const auto found = params.find(name);
    if (found == params.end() || found->is_null())
    {
        return std::nullopt;
    }
    if (auto value = detail::convert<T>(*found))
    {
        return value;
    }
    return std::unexpected(badParams("'" + std::string(name) + "' must be " +
                                     std::string(detail::expectedKind<T>()) + "."));
}

// The parameter; kBadParams when it is absent, null, or the wrong type.
template <typename T>
Result<T> requireParam(const json& params, std::string_view name)
{
    auto value = optionalParam<T>(params, name);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!value->has_value())
    {
        return std::unexpected(badParams("'" + std::string(name) + "' is required (" +
                                         std::string(detail::expectedKind<T>()) + ")."));
    }
    return std::move(**value);
}

// The first failure among several reads, so a handler can read every parameter
// up front and refuse once:
//
//   const auto key = requireParam<std::string>(params, "zenoh_key");
//   const auto field = optionalParam<std::string>(params, "field");
//   if (auto error = firstError(key, field)) return std::unexpected(*error);
template <typename... Results>
std::optional<AgentError> firstError(const Results&... results)
{
    std::optional<AgentError> first;
    ((first || results.has_value() ? void() : void(first = results.error())), ...);
    return first;
}

}  // namespace agent_control

#endif  // AGENT_CONTROL_PARAMS_H_
