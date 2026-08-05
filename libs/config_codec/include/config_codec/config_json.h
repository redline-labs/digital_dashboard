#ifndef DASHBOARD_CONFIG_JSON_H_
#define DASHBOARD_CONFIG_JSON_H_

#include "helpers/color.h"
#include "reflection/reflection.h"

#include <nlohmann/json.hpp>

#include <string>
#include <type_traits>
#include <vector>

namespace dashboard::config_json
{

using json = nlohmann::json;

// Generic JSON view of any REFLECT_STRUCT config, driven entirely by
// reflection::visit_fields.
//
// This is the same machinery dashboard/editor/properties_panel.cpp uses to build
// its forms, applied to a different output. Doing it generically is what makes
// the agent interface cover every widget automatically: a new widget with a
// reflected config becomes inspectable and settable with no work here.

// Detects a REFLECT_ENUM: the macro emits enum_names/enum_values found by ADL.
template <typename T, typename = void>
struct is_reflected_enum : std::false_type
{
};

template <typename T>
struct is_reflected_enum<T, std::void_t<decltype(enum_names(std::declval<T>()))>>
    : std::is_enum<T>::type
{
};

template <typename T, typename = void>
struct is_std_vector : std::false_type
{
};

template <typename T>
struct is_std_vector<std::vector<T>> : std::true_type
{
};

// ---------------------------------------------------------------- to JSON

template <typename T>
json toJson(const T& value);

template <typename T>
json leafToJson(const T& value)
{
    using U = std::decay_t<T>;

    if constexpr (std::is_same_v<U, helpers::Color>)
    {
        // A Color is a hex string on the wire; the editor knows to show a
        // picker for it, and describe() reports the distinct type so an agent
        // can too.
        return value.value();
    }
    else if constexpr (std::is_same_v<U, std::string>)
    {
        return value;
    }
    else if constexpr (is_reflected_enum<U>::value)
    {
        return std::string(reflection::enum_to_string(value));
    }
    else if constexpr (std::is_same_v<U, bool>)
    {
        return value;
    }
    else if constexpr (std::is_integral_v<U>)
    {
        return value;
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        return value;
    }
    else if constexpr (is_std_vector<U>::value)
    {
        json array = json::array();
        for (const auto& element : value)
        {
            array.push_back(toJson(element));
        }
        return array;
    }
    else if constexpr (reflection::is_reflected_struct<U>::value)
    {
        return toJson(value);
    }
    else
    {
        static_assert(sizeof(U) == 0,
                      "config_json: unsupported field type. Add a case here, or the "
                      "field will be silently missing from the agent interface.");
        return {};
    }
}

template <typename T>
json toJson(const T& value)
{
    if constexpr (reflection::is_reflected_struct<std::decay_t<T>>::value)
    {
        json out = json::object();
        reflection::visit_fields<std::decay_t<T>>(
            value,
            [&out](std::string_view name, const auto& field, std::string_view /*type*/)
            { out[std::string(name)] = leafToJson(field); });
        return out;
    }
    else
    {
        return leafToJson(value);
    }
}

// -------------------------------------------------------------- from JSON

// Applies `patch` onto `target`, touching only the fields present. Returns the
// problems found; an empty vector means every field applied.
//
// Partial-by-design: an agent adjusting one colour should not have to resend the
// whole config, which it would have to get exactly right or silently reset
// fields it never meant to touch.
template <typename T>
void applyJson(const json& patch, T& target, const std::string& path,
               std::vector<std::string>& errors);

template <typename T>
void applyLeaf(const json& value, T& target, const std::string& path,
               std::vector<std::string>& errors)
{
    using U = std::decay_t<T>;

    if constexpr (std::is_same_v<U, helpers::Color>)
    {
        if (!value.is_string())
        {
            errors.push_back(path + ": expected a hex colour string like \"#ff8800\".");
            return;
        }
        target = helpers::Color(value.get<std::string>());
    }
    else if constexpr (std::is_same_v<U, std::string>)
    {
        if (!value.is_string())
        {
            errors.push_back(path + ": expected a string.");
            return;
        }
        target = value.get<std::string>();
    }
    else if constexpr (is_reflected_enum<U>::value)
    {
        if (!value.is_string())
        {
            errors.push_back(path + ": expected one of the enum's string names.");
            return;
        }
        try
        {
            target = reflection::enum_traits<U>::from_string(value.get<std::string>());
        }
        catch (const std::exception&)
        {
            // Listing the valid names matters more than the failure itself --
            // an agent that guessed a name can only recover if told the set.
            std::string valid;
            for (const auto& name : reflection::enum_traits<U>::names())
            {
                valid += (valid.empty() ? "" : ", ");
                valid += std::string(name);
            }
            errors.push_back(path + ": '" + value.get<std::string>() +
                             "' is not a valid value. Expected one of: " + valid + ".");
        }
    }
    else if constexpr (std::is_same_v<U, bool>)
    {
        if (!value.is_boolean())
        {
            errors.push_back(path + ": expected a boolean.");
            return;
        }
        target = value.get<bool>();
    }
    else if constexpr (std::is_integral_v<U>)
    {
        if (!value.is_number_integer())
        {
            errors.push_back(path + ": expected an integer.");
            return;
        }
        target = value.get<U>();
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        if (!value.is_number())
        {
            errors.push_back(path + ": expected a number.");
            return;
        }
        target = value.get<U>();
    }
    else if constexpr (is_std_vector<U>::value)
    {
        if (!value.is_array())
        {
            errors.push_back(path + ": expected an array.");
            return;
        }
        // Replace wholesale. A per-element patch would need index semantics for
        // insert/remove that nothing here needs yet, and half-applying an array
        // is worse than replacing it.
        U replacement;
        replacement.resize(value.size());
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            applyJson(value[i], replacement[i], path + "[" + std::to_string(i) + "]", errors);
        }
        target = std::move(replacement);
    }
    else if constexpr (reflection::is_reflected_struct<U>::value)
    {
        applyJson(value, target, path, errors);
    }
    else
    {
        static_assert(sizeof(U) == 0, "config_json: unsupported field type in applyLeaf.");
    }
}

template <typename T>
void applyJson(const json& patch, T& target, const std::string& path,
               std::vector<std::string>& errors)
{
    if constexpr (reflection::is_reflected_struct<std::decay_t<T>>::value)
    {
        if (!patch.is_object())
        {
            errors.push_back((path.empty() ? std::string("<root>") : path) +
                             ": expected an object.");
            return;
        }

        // Reject unknown keys rather than ignoring them. A misspelled field that
        // silently does nothing is the worst outcome here: the caller believes
        // it changed something and the UI disagrees.
        std::vector<std::string> known;
        reflection::visit_fields<std::decay_t<T>>(
            target, [&known](std::string_view name, const auto&, std::string_view)
            { known.emplace_back(name); });

        for (const auto& [key, unused] : patch.items())
        {
            if (std::find(known.begin(), known.end(), key) == known.end())
            {
                std::string valid;
                for (const auto& name : known)
                {
                    valid += (valid.empty() ? "" : ", ");
                    valid += name;
                }
                errors.push_back((path.empty() ? key : path + "." + key) +
                                 ": no such field. Known fields: " + valid + ".");
            }
        }

        reflection::visit_fields<std::decay_t<T>>(
            target,
            [&](std::string_view name, auto& field, std::string_view /*type*/)
            {
                const std::string key(name);
                if (!patch.contains(key))
                {
                    return;
                }
                applyLeaf(patch[key], field, path.empty() ? key : path + "." + key, errors);
            });
    }
    else
    {
        applyLeaf(patch, target, path, errors);
    }
}

// -------------------------------------------------------------- describe

template <typename T>
json describeType();

template <typename T>
json describeLeafType()
{
    using U = std::decay_t<T>;
    json out = json::object();

    if constexpr (std::is_same_v<U, helpers::Color>)
    {
        out["type"] = "color";
        out["format"] = "#rrggbb";
    }
    else if constexpr (std::is_same_v<U, std::string>)
    {
        out["type"] = "string";
    }
    else if constexpr (is_reflected_enum<U>::value)
    {
        out["type"] = "enum";
        json values = json::array();
        for (const auto& name : reflection::enum_traits<U>::names())
        {
            values.push_back(std::string(name));
        }
        out["values"] = std::move(values);
    }
    else if constexpr (std::is_same_v<U, bool>)
    {
        out["type"] = "bool";
    }
    else if constexpr (std::is_integral_v<U>)
    {
        out["type"] = "int";
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        out["type"] = "number";
    }
    else if constexpr (is_std_vector<U>::value)
    {
        out["type"] = "array";
        out["items"] = describeType<typename U::value_type>();
    }
    else if constexpr (reflection::is_reflected_struct<U>::value)
    {
        out = describeType<U>();
    }
    else
    {
        static_assert(sizeof(U) == 0, "config_json: unsupported field type in describe.");
    }

    return out;
}

// Field names, types, and the friendly names/descriptions the widget author
// wrote in REFLECT_METADATA -- so an agent can discover what is settable instead
// of guessing and getting a "no such field" error.
template <typename T>
json describeType()
{
    using U = std::decay_t<T>;

    if constexpr (reflection::is_reflected_struct<U>::value)
    {
        json fields = json::object();

        // visit_fields needs an instance; a default-constructed one also gives
        // the defaults, which are worth reporting.
        U sample{};
        reflection::visit_fields<U>(
            sample,
            [&fields](std::string_view name, const auto& field, std::string_view type_name)
            {
                json entry = describeLeafType<std::decay_t<decltype(field)>>();
                entry["cpp_type"] = std::string(type_name);
                entry["default"] = leafToJson(field);

                const auto friendly = reflection::get_friendly_name<U>(name);
                if (!friendly.empty() && friendly != name)
                {
                    entry["title"] = std::string(friendly);
                }
                const auto description = reflection::get_description<U>(name);
                if (!description.empty())
                {
                    entry["description"] = std::string(description);
                }

                fields[std::string(name)] = std::move(entry);
            });

        json out = json::object();
        out["type"] = "object";
        out["fields"] = std::move(fields);
        return out;
    }
    else
    {
        return describeLeafType<U>();
    }
}

}  // namespace dashboard::config_json

#endif  // DASHBOARD_CONFIG_JSON_H_
