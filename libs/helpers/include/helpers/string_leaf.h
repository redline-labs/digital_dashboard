#ifndef HELPERS_STRING_LEAF_H
#define HELPERS_STRING_LEAF_H

#include <concepts>
#include <string>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace helpers
{

// A config field that is a string on the wire but has rules of its own: a
// colour, a zenoh topic key, a service key.
//
// The codecs, the validator and both config forms handle every one of these
// through this concept, keyed on the field's TYPE. Before it, colour was a
// special case in seven places, and which strings were zenoh keys was guessed
// from the field's name -- so a key not named `*zenoh_key` was never checked.
//
// `problem(text)` is empty when the text is acceptable, otherwise the reason it
// is not, phrased to follow the quoted value: "'#GG' is not a colour; ...".
template <typename T>
concept StringLeaf = requires(const T& leaf, const std::string& text, std::string_view view) {
    { leaf.str() } -> std::same_as<const std::string&>;
    T{text};
    { T::problem(view) } -> std::same_as<std::string>;
    // What describe() reports as the field's type, and a short hint at the
    // accepted form, for an agent or a form placeholder.
    { T::kTypeName } -> std::convertible_to<std::string_view>;
    { T::kFormatHint } -> std::convertible_to<std::string_view>;
};

}  // namespace helpers

namespace YAML
{

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

}  // namespace YAML

#endif  // HELPERS_STRING_LEAF_H
