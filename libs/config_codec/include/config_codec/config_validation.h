#ifndef CONFIG_CODEC_CONFIG_VALIDATION_H_
#define CONFIG_CODEC_CONFIG_VALIDATION_H_

#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "helpers/color.h"
#include "reflection/reflection.h"

namespace config_codec {

// One problem found in a config file, with the path that leads to it.
//
// The path is the whole point. Before this existed, a mistyped key was dropped
// without a word -- `zenoh_kye:` produced a widget subscribed to "", which
// showed up three layers away as a permanently blank gauge -- and a wrong type
// aborted the entire file with a yaml-cpp message that named neither the field
// nor the widget. "widgets[3].config.zenoh_key" is the difference between a
// two-minute fix and an afternoon.
struct Issue
{
    enum class Severity
    {
        // The value is unusable. Loading cannot produce what the file asked for.
        error,

        // The file says something the program does not understand, but loading
        // can continue. Almost always a typo or a leftover from a removed
        // feature.
        warning,
    };

    Severity severity;
    std::string path;
    std::string message;
};

namespace detail {

inline std::string join(const std::string& path, std::string_view field)
{
    return path.empty() ? std::string(field) : path + "." + std::string(field);
}

template <typename Struct>
std::vector<std::string> fieldNames()
{
    std::vector<std::string> names;
    Struct probe{};
    reflection::visit_fields(probe, [&](std::string_view name, auto&, std::string_view)
    {
        names.emplace_back(name);
    });
    return names;
}

inline std::string commaJoin(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out;
}

// Walks a YAML node against a reflected struct, reporting keys the struct does
// not have and values it cannot accept.
template <typename Struct>
void validateStruct(const YAML::Node& node, const std::string& path, std::vector<Issue>& issues)
{
    if (!node.IsMap())
    {
        issues.push_back({Issue::Severity::error, path, "expected a mapping here"});
        return;
    }

    const std::vector<std::string> known = fieldNames<Struct>();

    // Keys the struct does not have. The decoder is driven by the struct's
    // fields, so these were silently discarded.
    for (const auto& entry : node)
    {
        const std::string key = entry.first.as<std::string>();
        if (std::find(known.begin(), known.end(), key) == known.end())
        {
            issues.push_back({Issue::Severity::warning, join(path, key),
                              "unknown key, ignored; this struct has: " + commaJoin(known)});
        }
    }

    // Values the struct cannot accept, and a recursive walk of nested structs.
    Struct probe{};
    reflection::visit_fields(probe, [&](std::string_view name, auto& ref, std::string_view)
    {
        using Field = std::decay_t<decltype(ref)>;
        const std::string field_path = join(path, name);
        const YAML::Node child = node[std::string(name)];
        if (!child)
        {
            // Absent means "use the default", which is legitimate everywhere in
            // this schema -- there are no required fields below the top level.
            return;
        }

        if constexpr (reflection::is_reflected_struct<Field>::value)
        {
            validateStruct<Field>(child, field_path, issues);
        }
        else if constexpr (reflection::is_std_vector<Field>::value)
        {
            using Elem = typename reflection::is_std_vector<Field>::value_type;
            if (!child.IsSequence())
            {
                issues.push_back({Issue::Severity::error, field_path, "expected a list"});
                return;
            }
            for (std::size_t i = 0; i < child.size(); ++i)
            {
                const std::string elem_path = field_path + "[" + std::to_string(i) + "]";
                if constexpr (reflection::is_reflected_struct<Elem>::value)
                {
                    validateStruct<Elem>(child[i], elem_path, issues);
                }
                else
                {
                    try
                    {
                        (void)child[i].as<Elem>();
                    }
                    catch (const std::exception& e)
                    {
                        issues.push_back({Issue::Severity::error, elem_path, e.what()});
                    }
                }
            }
        }
        else if constexpr (std::is_same_v<Field, helpers::Color>)
        {
            // An unparseable colour reaches QColor as "invalid", which paints as
            // transparent black or -- where the value goes into a stylesheet --
            // makes Qt drop the rule entirely. Both are silent, so catch the
            // typo here where the field's path can be named.
            std::string text;
            try
            {
                text = child.as<std::string>();
            }
            catch (const std::exception&)
            {
                issues.push_back({Issue::Severity::error, field_path, "expected a colour string"});
                return;
            }

            if (!helpers::Color::isValidFormat(text))
            {
                issues.push_back({Issue::Severity::error, field_path,
                                  "'" + text + "' is not a colour; expected #RGB, #RRGGBB or #RRGGBBAA"});
            }
        }
        else if constexpr (std::is_enum_v<Field>)
        {
            // Handled here rather than through child.as<Field>() because
            // yaml-cpp replaces a failed conversion's message with a bare "bad
            // conversion", losing both the offending value and the alternatives.
            std::string text;
            try
            {
                text = child.as<std::string>();
            }
            catch (const std::exception&)
            {
                issues.push_back({Issue::Severity::error, field_path, "expected a name, not a collection"});
                return;
            }

            if (!reflection::enum_traits<Field>::try_from_string(text))
            {
                issues.push_back({Issue::Severity::error, field_path,
                                  "unknown value '" + text + "'; expected one of: " +
                                      reflection::enum_traits<Field>::known_values()});
            }
        }
        else
        {
            // Ask the same converter the loader will use, so validation cannot
            // disagree with the load that follows it.
            try
            {
                (void)child.as<Field>();
            }
            catch (const std::exception& e)
            {
                issues.push_back({Issue::Severity::error, field_path, e.what()});
            }
        }
    });
}

}  // namespace detail
}  // namespace config_codec

#endif  // CONFIG_CODEC_CONFIG_VALIDATION_H_
