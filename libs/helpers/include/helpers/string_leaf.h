#ifndef HELPERS_STRING_LEAF_H
#define HELPERS_STRING_LEAF_H

#include <concepts>
#include <string>
#include <string_view>

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
//
// No yaml-cpp here: the YAML conversion is in config_codec/config_yaml.h, with
// the rest of the codec, so a key or a colour can be used without it.
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


#endif  // HELPERS_STRING_LEAF_H
