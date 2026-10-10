#ifndef NODE_CONFIG_READER_H_
#define NODE_CONFIG_READER_H_

#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include <yaml-cpp/yaml.h>

namespace node_config
{

// The small set of readers every node's hand-written YAML parser shares.
//
// Each node parses its own config by hand -- they hold Eigen vectors, external
// enums and optionals that the reflected codec does not -- but the leaf reads
// were copied into eleven parsers, with drifting messages and one that took its
// arguments in a different order. These are that copy, once.
//
// The shape every reader shares: an absent key leaves `out` alone (the field
// keeps its default), and a present key that does not convert records a failure
// naming the field and carries on, so a config with three mistakes is reported
// in one run rather than three.

// Accumulates failures: each is logged as it is found, and `ok` says whether any
// was. A node's parse function returns nullopt when !ok.
struct Context
{
    bool ok{true};

    void fail(const std::string& message);
};

// "where.key", or just "key" at the top level. A `where` that already ends in
// '.' is used as given.
std::string fieldPath(const std::string& where, const char* key);

void readString(const YAML::Node& parent, const char* key, std::string& out, Context& context,
                const std::string& where);

void readBool(const YAML::Node& parent, const char* key, bool& out, Context& context,
              const std::string& where);

// An unsigned field, range-checked against T: 300 into a uint8_t is refused
// rather than wrapped to 44, and a negative number is refused rather than
// wrapped to a huge one.
template <std::unsigned_integral T>
void readUint(const YAML::Node& parent, const char* key, T& out, Context& context, const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    std::uint64_t value = 0;
    try
    {
        // yaml-cpp refuses a negative number for an unsigned type itself;
        // node_config_test_reader pins that.
        value = node.as<std::uint64_t>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(fieldPath(where, key) + " must be a non-negative integer");
        return;
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
    {
        context.fail(fieldPath(where, key) + ": " + std::to_string(value) + " is out of range");
        return;
    }
    out = static_cast<T>(value);  // range-checked just above
}

// Any numeric field. An unsigned one is read as readUint reads it; a
// floating-point one must also be finite: `.inf` parses, and then poisons every
// computation it reaches.
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
void readNumber(const YAML::Node& parent, const char* key, T& out, Context& context, const std::string& where)
{
    if constexpr (std::is_unsigned_v<T>)
    {
        readUint(parent, key, out, context, where);
        return;
    }
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    T value{};
    try
    {
        value = node.as<T>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(fieldPath(where, key) + " must be a number");
        return;
    }
    if constexpr (std::is_floating_point_v<T>)
    {
        if (!std::isfinite(value))
        {
            context.fail(fieldPath(where, key) + " must be finite");
            return;
        }
    }
    out = value;
}

// A zenoh key a node publishes or subscribes on. Checked at load rather than at
// the publisher, because a bad key does not fail loudly on the bus: `*` and `?`
// are refused by zenoh outright and `@` makes a segment verbatim, so the topic
// is silently never seen. `field` names it in the message.
void checkTopicKey(const std::string& key, const std::string& field, Context& context);

}  // namespace node_config

#endif  // NODE_CONFIG_READER_H_
