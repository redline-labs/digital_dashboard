// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/bplist.ts
#ifndef AIRPLAY_PLIST_H_
#define AIRPLAY_PLIST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace airplay::plist
{

using Bytes = std::vector<uint8_t>;

// The subset of Apple's binary property list ("bplist00") that CarPlay's
// RTSP-style control channel uses: dicts, arrays, ASCII/UTF-16 strings, raw
// data, integers, reals, booleans and dates.
//
// Value is a tagged union rather than a std::variant so that the recursive
// container members only ever need std::vector of an incomplete type, which is
// the one form the standard blesses.
class Value
{
public:
    enum class Type
    {
        Null,
        Bool,
        Integer,
        Real,
        String,
        Data,
        Date,
        Array,
        Dict,
    };

    Value() = default;

    static Value boolean(bool value);
    static Value integer(int64_t value);
    static Value real(double value);
    static Value string(std::string value);
    static Value data(Bytes value);
    // Apple epoch: seconds since 2001-01-01T00:00:00Z.
    static Value date(double seconds_since_2001);
    static Value array(std::vector<Value> values = {});
    static Value dict();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isInteger() const { return type_ == Type::Integer; }
    bool isReal() const { return type_ == Type::Real; }
    bool isString() const { return type_ == Type::String; }
    bool isData() const { return type_ == Type::Data; }
    bool isDate() const { return type_ == Type::Date; }
    bool isArray() const { return type_ == Type::Array; }
    bool isDict() const { return type_ == Type::Dict; }

    // Accessors return the fallback when the type does not match, so callers
    // parsing a phone's payload never have to guard first.
    bool asBool(bool fallback = false) const;
    int64_t asInteger(int64_t fallback = 0) const;
    // Accepts Integer as well, since bplist writers freely mix the two.
    double asReal(double fallback = 0.0) const;
    double asDate(double fallback = 0.0) const;
    const std::string& asString() const;
    const Bytes& asData() const;

    // --- Array ---
    size_t size() const;  // element count for arrays, entry count for dicts
    const Value& at(size_t index) const;
    void push(Value value);

    // --- Dict ---
    // Insertion ordered; setting an existing key replaces it in place so that
    // encoding is deterministic.
    void set(std::string key, Value value);
    const Value* find(std::string_view key) const;
    bool contains(std::string_view key) const;
    const std::vector<std::string>& keys() const { return keys_; }
    const Value& valueAt(size_t index) const;

    bool operator==(const Value& other) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    int64_t integer_ = 0;
    double real_ = 0.0;
    std::string string_;
    Bytes data_;
    std::vector<std::string> keys_;  // dict keys, parallel to children_
    std::vector<Value> children_;    // array elements or dict values
};

// Serializes to a complete bplist00 document. Returns an empty vector on
// failure (only possible for pathologically large inputs).
Bytes encode(const Value& root);

// Parses a bplist00 document. Returns nullopt on a malformed or truncated
// buffer, or on an object type outside the supported subset.
std::optional<Value> decode(const Bytes& buffer);

}  // namespace airplay::plist

#endif  // AIRPLAY_PLIST_H_
