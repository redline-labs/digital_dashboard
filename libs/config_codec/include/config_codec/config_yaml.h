#ifndef CONFIG_CODEC_CONFIG_YAML_H_
#define CONFIG_CODEC_CONFIG_YAML_H_

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "helpers/string_leaf.h"
#include "reflection/reflection.h"

#include <yaml-cpp/yaml.h>

// YAML conversion and equality for every REFLECT_STRUCT and REFLECT_ENUM, with
// no per-type registration anywhere.
//
// This lived in dashboard/app_config.h next to app_config_t and widget_config_t.
// Nothing in it is about a dashboard: it is generic over reflection, and the
// scope app needs the same behaviour for its own workspace and panel configs.
// The widget-specific pieces -- the widget_config_t full specialization and the
// widget table it walks -- stayed behind.

// Equality for anything declared with REFLECT_STRUCT, derived from the fields
// the same way the YAML conversion below is.
//
// The editor needs this to answer "did anything change?" without serialising the
// whole document to YAML and comparing strings -- which is what it used to do on
// every drag release and every title-bar update. It is also what lets an undo
// work out which widgets actually differ, so it can put one back where it was
// instead of rebuilding all of them.
//
// Writing these by hand was rejected once, reasonably: an operator== per widget
// config struct is a list that rots. Generated from the reflection data it
// cannot, and std::variant picks its own up for free once every alternative has
// one -- which is what makes widget_config_t's comparison three lines rather
// than a switch over the widget table.
// Declared ahead of fieldsEqual so that a nested field's comparison finds it by
// ordinary lookup. ADL alone does not: for a reflected struct declared inside a
// namespace (pub_sub::subscription_t) it searches only that namespace.
template <typename T>
    requires reflection::is_reflected_struct_v<T>
bool operator==(const T& lhs, const T& rhs);

namespace config_codec::detail
{
// Compares field-by-field through the member pointers reflection already holds,
// rather than by walking both objects with visit_fields and matching names --
// which would be quadratic and would need the field types proved equal at each
// step. The fold short-circuits, so an early difference costs one comparison.
template <typename T, std::size_t... I>
bool fieldsEqual(const T& lhs, const T& rhs, std::index_sequence<I...>)
{
    const auto fields = T::reflection_fields();
    return (... && (lhs.*(std::get<I>(fields).member_ptr) ==
                    rhs.*(std::get<I>(fields).member_ptr)));
}
}  // namespace config_codec::detail

template <typename T>
    requires reflection::is_reflected_struct_v<T>
bool operator==(const T& lhs, const T& rhs)
{
    constexpr std::size_t kFieldCount =
        std::tuple_size_v<decltype(T::reflection_fields())>;
    return config_codec::detail::fieldsEqual(lhs, rhs,
                                             std::make_index_sequence<kFieldCount>{});
}

namespace config_codec
{
// Fields the YAML encoder leaves out while they hold their default.
//
// For a key added after files were written without it: every config saved
// before windows had a display would otherwise gain `display: primary` the
// first time the editor saved it, which is noise in every diff. Specialise
// with the field names; the decode is unaffected, since an absent key keeps
// its default anyway.
//
//     template <> struct yaml_omit_when_default<app_config_t>
//     {
//         static constexpr std::string_view fields[] = {"display", "scale"};
//     };
template <typename T>
struct yaml_omit_when_default
{
    static constexpr std::string_view fields[] = {""};
};

namespace detail
{
template <typename T>
constexpr bool omittedWhenDefault(std::string_view field)
{
    for (const std::string_view name : yaml_omit_when_default<T>::fields)
    {
        if (!name.empty() && name == field)
        {
            return true;
        }
    }
    return false;
}

template <typename T>
constexpr bool hasOmissions()
{
    for (const std::string_view name : yaml_omit_when_default<T>::fields)
    {
        if (!name.empty())
        {
            return true;
        }
    }
    return false;
}

template <typename T, std::size_t... I>
void encodeFields(YAML::Node& node, const T& rhs, std::index_sequence<I...>)
{
    [[maybe_unused]] static const T defaults{};
    const auto fields = T::reflection_fields();
    (
        [&] {
            const auto& field = std::get<I>(fields);
            const auto& value = rhs.*(field.member_ptr);
            // Only a struct that names omissions compares its fields, so the
            // rest need no operator== on every field type.
            if constexpr (hasOmissions<T>())
            {
                if (omittedWhenDefault<T>(field.name) && value == defaults.*(field.member_ptr))
                {
                    return;
                }
            }
            node[std::string(field.name)] = value;
        }(),
        ...);
}
}  // namespace detail
}  // namespace config_codec

namespace YAML {

// Every REFLECT_STRUCT and REFLECT_ENUM converts to and from YAML, without
// being named anywhere.
//
// These used to be per-type macro invocations -- one line for each widget
// config, each nested struct and each enum. That list was pure duplication of
// what the reflection macros already declare, and it failed open: leaving a
// type out is not a missing-registration error but an "implicit instantiation
// of undefined template" from deep inside yaml-cpp, pointing at the header that
// happened to instantiate it rather than at the type. Nested structs and enums
// were the usual casualty, because adding one to an existing config does not
// look like it should need registering anywhere.
//
// The argument list here is identical to yaml-cpp's primary template, which is
// normally ill-formed for a partial specialization. C++20 permits it when the
// specialization is more constrained ([temp.spec.partial]), which is what the
// requires-clauses provide. Hand-written full specializations -- widget_config_t
// in app_config.h -- are more specialized still and continue to win. String
// leaves (colours, keys) have their own, below.
template <typename T>
    requires reflection::is_reflected_struct_v<T>
struct convert<T>
{
    static Node encode(const T& rhs)
    {
        Node node = {};
        constexpr std::size_t kFieldCount = std::tuple_size_v<decltype(T::reflection_fields())>;
        config_codec::detail::encodeFields(node, rhs, std::make_index_sequence<kFieldCount>{});
        return node;
    }

    static bool decode(const Node& node, T& rhs)
    {
        if (!node.IsMap()) return false;
        reflection::visit_fields<T>(rhs, [&](std::string_view name, auto& ref, std::string_view /*type*/)
        {
            if (node[name])
            {
                ref = node[name].as<std::decay_t<decltype(ref)>>();
            }
        });
        return true;
    }
};

template <typename T>
    requires reflection::is_reflected_enum_v<T>
struct convert<T>
{
    static Node encode(const T& rhs)
    {
        return YAML::Node(reflection::enum_to_string(rhs));
    }

    static bool decode(const Node& node, T& rhs)
    {
        // Silent on failure by design: the caller's validation pass reports the
        // same problem with the field's path and the valid alternatives, and two
        // messages for one typo is worse than one good one.
        try
        {
            const auto value = reflection::enum_traits<T>::try_from_string(node.as<std::string>());
            if (!value) return false;
            rhs = *value;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
};

// A string leaf is a plain scalar in YAML. Its rules are checked by the
// validator, which can name the field; the decode only refuses a non-scalar.
template <helpers::StringLeaf T>
struct convert<T>
{
    static Node encode(const T& rhs) { return Node(rhs.str()); }

    static bool decode(const Node& node, T& rhs)
    {
        if (!node.IsScalar())
        {
            return false;
        }
        rhs = T{node.as<std::string>()};
        return true;
    }
};

}   // namespace YAML

#endif  // CONFIG_CODEC_CONFIG_YAML_H_
