// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/bplist.ts
#include "airplay/plist.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace airplay::plist
{

namespace
{

const std::string kEmptyString;
const Bytes kEmptyBytes;
const Value kNullValue;

constexpr uint8_t kMagic[8] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
constexpr size_t kTrailerSize = 32;
constexpr size_t kMaxDepth = 32;
constexpr size_t kMaxVisits = 1u << 20;

// --- Bit twiddling ----------------------------------------------------------

void appendBigEndian(Bytes& out, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        out.push_back(static_cast<uint8_t>((value >> (8 * (size - 1 - i))) & 0xff));
    }
}

uint64_t doubleToBits(double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double bitsToDouble(uint64_t bits)
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float bitsToFloat(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- UTF-8 <-> UTF-16 -------------------------------------------------------

bool isAscii(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<uint8_t>(c) <= 0x7f; });
}

// Decodes UTF-8 into UTF-16 code units. Returns false on malformed input.
bool utf8ToUtf16(const std::string& in, std::vector<uint16_t>& out)
{
    size_t i = 0;
    while (i < in.size())
    {
        const uint8_t c = static_cast<uint8_t>(in[i]);
        uint32_t code = 0;
        size_t extra = 0;
        if (c < 0x80)
        {
            code = c;
        }
        else if ((c & 0xe0) == 0xc0)
        {
            code = c & 0x1fu;
            extra = 1;
        }
        else if ((c & 0xf0) == 0xe0)
        {
            code = c & 0x0fu;
            extra = 2;
        }
        else if ((c & 0xf8) == 0xf0)
        {
            code = c & 0x07u;
            extra = 3;
        }
        else
        {
            return false;
        }

        if (i + extra >= in.size())
        {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k)
        {
            const uint8_t cc = static_cast<uint8_t>(in[i + k]);
            if ((cc & 0xc0) != 0x80)
            {
                return false;
            }
            code = (code << 6) | (cc & 0x3fu);
        }
        i += extra + 1;

        if (code > 0x10ffff)
        {
            return false;
        }
        if (code >= 0x10000)
        {
            const uint32_t v = code - 0x10000;
            out.push_back(static_cast<uint16_t>(0xd800 + (v >> 10)));
            out.push_back(static_cast<uint16_t>(0xdc00 + (v & 0x3ff)));
        }
        else
        {
            out.push_back(static_cast<uint16_t>(code));
        }
    }
    return true;
}

void appendUtf8(std::string& out, uint32_t code)
{
    if (code < 0x80)
    {
        out.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
    else if (code < 0x10000)
    {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
    else
    {
        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

// bplist unicode strings are UTF-16 big-endian.
std::string utf16BeToUtf8(const uint8_t* p, size_t units)
{
    std::string out;
    out.reserve(units);
    for (size_t i = 0; i < units; ++i)
    {
        uint32_t code = (static_cast<uint32_t>(p[i * 2]) << 8) | p[i * 2 + 1];
        if (code >= 0xd800 && code <= 0xdbff && i + 1 < units)
        {
            const uint32_t low = (static_cast<uint32_t>(p[(i + 1) * 2]) << 8) | p[(i + 1) * 2 + 1];
            if (low >= 0xdc00 && low <= 0xdfff)
            {
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            }
        }
        appendUtf8(out, code);
    }
    return out;
}

}  // namespace

// --- Value ------------------------------------------------------------------

Value Value::boolean(bool value)
{
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = value;
    return v;
}

Value Value::integer(int64_t value)
{
    Value v;
    v.type_ = Type::Integer;
    v.integer_ = value;
    return v;
}

Value Value::real(double value)
{
    Value v;
    v.type_ = Type::Real;
    v.real_ = value;
    return v;
}

Value Value::string(std::string value)
{
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(value);
    return v;
}

Value Value::data(Bytes value)
{
    Value v;
    v.type_ = Type::Data;
    v.data_ = std::move(value);
    return v;
}

Value Value::date(double seconds_since_2001)
{
    Value v;
    v.type_ = Type::Date;
    v.real_ = seconds_since_2001;
    return v;
}

Value Value::array(std::vector<Value> values)
{
    Value v;
    v.type_ = Type::Array;
    v.children_ = std::move(values);
    return v;
}

Value Value::dict()
{
    Value v;
    v.type_ = Type::Dict;
    return v;
}

bool Value::asBool(bool fallback) const
{
    return type_ == Type::Bool ? bool_ : fallback;
}

int64_t Value::asInteger(int64_t fallback) const
{
    if (type_ == Type::Integer)
    {
        return integer_;
    }
    if (type_ == Type::Real)
    {
        return static_cast<int64_t>(real_);
    }
    return fallback;
}

double Value::asReal(double fallback) const
{
    if (type_ == Type::Real)
    {
        return real_;
    }
    if (type_ == Type::Integer)
    {
        return static_cast<double>(integer_);
    }
    return fallback;
}

double Value::asDate(double fallback) const
{
    return type_ == Type::Date ? real_ : fallback;
}

const std::string& Value::asString() const
{
    return type_ == Type::String ? string_ : kEmptyString;
}

const Bytes& Value::asData() const
{
    return type_ == Type::Data ? data_ : kEmptyBytes;
}

size_t Value::size() const
{
    return children_.size();
}

const Value& Value::at(size_t index) const
{
    return index < children_.size() ? children_[index] : kNullValue;
}

void Value::push(Value value)
{
    if (type_ != Type::Array)
    {
        type_ = Type::Array;
        keys_.clear();
    }
    children_.push_back(std::move(value));
}

void Value::set(std::string key, Value value)
{
    if (type_ != Type::Dict)
    {
        type_ = Type::Dict;
        children_.clear();
        keys_.clear();
    }

    for (size_t i = 0; i < keys_.size(); ++i)
    {
        if (keys_[i] == key)
        {
            children_[i] = std::move(value);
            return;
        }
    }
    keys_.push_back(std::move(key));
    children_.push_back(std::move(value));
}

const Value* Value::find(std::string_view key) const
{
    if (type_ != Type::Dict)
    {
        return nullptr;
    }
    for (size_t i = 0; i < keys_.size(); ++i)
    {
        if (keys_[i] == key)
        {
            return &children_[i];
        }
    }
    return nullptr;
}

bool Value::contains(std::string_view key) const
{
    return find(key) != nullptr;
}

const Value& Value::valueAt(size_t index) const
{
    return index < children_.size() ? children_[index] : kNullValue;
}

bool Value::operator==(const Value& other) const
{
    if (type_ != other.type_)
    {
        return false;
    }
    switch (type_)
    {
        case Type::Null:
            return true;
        case Type::Bool:
            return bool_ == other.bool_;
        case Type::Integer:
            return integer_ == other.integer_;
        case Type::Real:
        case Type::Date:
            return real_ == other.real_;
        case Type::String:
            return string_ == other.string_;
        case Type::Data:
            return data_ == other.data_;
        case Type::Array:
            return children_ == other.children_;
        case Type::Dict:
            return keys_ == other.keys_ && children_ == other.children_;
    }
    return false;
}

// --- Encode -----------------------------------------------------------------

namespace
{

struct Node
{
    Bytes head;               // marker (+ inline body for leaves)
    std::vector<size_t> refs;  // container children, empty for leaves
};

// Object marker: the low nibble carries the count, or 0x0f plus a trailing int
// object when the count does not fit.
Bytes objectMarker(uint8_t type, size_t count)
{
    Bytes out;
    if (count < 0x0f)
    {
        out.push_back(static_cast<uint8_t>((type << 4) | count));
        return out;
    }

    const size_t bytes = count > 0xffff ? 4 : (count > 0xff ? 2 : 1);
    const uint8_t log = bytes == 4 ? 2 : (bytes == 2 ? 1 : 0);
    out.push_back(static_cast<uint8_t>((type << 4) | 0x0f));
    out.push_back(static_cast<uint8_t>(0x10 | log));
    appendBigEndian(out, count, bytes);
    return out;
}

Bytes encodeInteger(int64_t value)
{
    Bytes out;
    if (value < 0)
    {
        // Apple writes negative integers as a full 8-byte two's complement word.
        out.push_back(0x13);
        appendBigEndian(out, static_cast<uint64_t>(value), 8);
        return out;
    }

    const uint64_t v = static_cast<uint64_t>(value);
    const size_t bytes = v > 0xffffffffull ? 8 : (v > 0xffff ? 4 : (v > 0xff ? 2 : 1));
    const uint8_t log = bytes == 8 ? 3 : (bytes == 4 ? 2 : (bytes == 2 ? 1 : 0));
    out.push_back(static_cast<uint8_t>(0x10 | log));
    appendBigEndian(out, v, bytes);
    return out;
}

bool addNode(const Value& value, std::vector<Node>& nodes, size_t depth, size_t& index)
{
    if (depth > kMaxDepth)
    {
        SPDLOG_ERROR("[airplay] plist encode: nesting deeper than {} levels", kMaxDepth);
        return false;
    }

    index = nodes.size();
    nodes.emplace_back();  // reserve the slot before recursing into children

    switch (value.type())
    {
        case Value::Type::Null:
            nodes[index].head = Bytes{0x00};
            break;
        case Value::Type::Bool:
            nodes[index].head = Bytes{static_cast<uint8_t>(value.asBool() ? 0x09 : 0x08)};
            break;
        case Value::Type::Integer:
            nodes[index].head = encodeInteger(value.asInteger());
            break;
        case Value::Type::Real:
        {
            Bytes body{0x23};
            appendBigEndian(body, doubleToBits(value.asReal()), 8);
            nodes[index].head = std::move(body);
            break;
        }
        case Value::Type::Date:
        {
            Bytes body{0x33};
            appendBigEndian(body, doubleToBits(value.asDate()), 8);
            nodes[index].head = std::move(body);
            break;
        }
        case Value::Type::String:
        {
            const std::string& s = value.asString();
            if (isAscii(s))
            {
                Bytes body = objectMarker(0x5, s.size());
                body.insert(body.end(), s.begin(), s.end());
                nodes[index].head = std::move(body);
            }
            else
            {
                std::vector<uint16_t> units;
                if (!utf8ToUtf16(s, units))
                {
                    SPDLOG_ERROR("[airplay] plist encode: malformed UTF-8 string ({} bytes)", s.size());
                    return false;
                }
                Bytes body = objectMarker(0x6, units.size());
                for (uint16_t unit : units)
                {
                    body.push_back(static_cast<uint8_t>(unit >> 8));
                    body.push_back(static_cast<uint8_t>(unit & 0xff));
                }
                nodes[index].head = std::move(body);
            }
            break;
        }
        case Value::Type::Data:
        {
            const Bytes& d = value.asData();
            Bytes body = objectMarker(0x4, d.size());
            body.insert(body.end(), d.begin(), d.end());
            nodes[index].head = std::move(body);
            break;
        }
        case Value::Type::Array:
        {
            std::vector<size_t> refs;
            refs.reserve(value.size());
            for (size_t i = 0; i < value.size(); ++i)
            {
                size_t child = 0;
                if (!addNode(value.at(i), nodes, depth + 1, child))
                {
                    return false;
                }
                refs.push_back(child);
            }
            nodes[index].head = objectMarker(0xa, refs.size());
            nodes[index].refs = std::move(refs);
            break;
        }
        case Value::Type::Dict:
        {
            const auto& keys = value.keys();
            std::vector<size_t> refs;
            refs.reserve(keys.size() * 2);
            for (const auto& key : keys)
            {
                size_t child = 0;
                if (!addNode(Value::string(key), nodes, depth + 1, child))
                {
                    return false;
                }
                refs.push_back(child);
            }
            for (size_t i = 0; i < keys.size(); ++i)
            {
                size_t child = 0;
                if (!addNode(value.valueAt(i), nodes, depth + 1, child))
                {
                    return false;
                }
                refs.push_back(child);
            }
            nodes[index].head = objectMarker(0xd, keys.size());
            nodes[index].refs = std::move(refs);
            break;
        }
    }
    return true;
}

}  // namespace

Bytes encode(const Value& root)
{
    std::vector<Node> nodes;
    size_t top_index = 0;
    if (!addNode(root, nodes, 0, top_index))
    {
        return {};
    }

    const size_t num_objects = nodes.size();
    const size_t ref_size = num_objects > 0xffff ? 4 : (num_objects > 0xff ? 2 : 1);

    Bytes out(std::begin(kMagic), std::end(kMagic));
    std::vector<size_t> offsets;
    offsets.reserve(num_objects);
    for (const auto& node : nodes)
    {
        offsets.push_back(out.size());
        out.insert(out.end(), node.head.begin(), node.head.end());
        for (size_t ref : node.refs)
        {
            appendBigEndian(out, ref, ref_size);
        }
    }

    const size_t offset_table_offset = out.size();
    const size_t offset_size =
        offset_table_offset > 0xffffffffull ? 8 : (offset_table_offset > 0xffff ? 4 : (offset_table_offset > 0xff ? 2 : 1));
    for (size_t offset : offsets)
    {
        appendBigEndian(out, offset, offset_size);
    }

    Bytes trailer(kTrailerSize, 0);
    trailer[6] = static_cast<uint8_t>(offset_size);
    trailer[7] = static_cast<uint8_t>(ref_size);
    Bytes tail;
    appendBigEndian(tail, num_objects, 8);
    appendBigEndian(tail, top_index, 8);
    appendBigEndian(tail, offset_table_offset, 8);
    std::copy(tail.begin(), tail.end(), trailer.begin() + 8);
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

// --- Decode -----------------------------------------------------------------

namespace
{

class Decoder
{
public:
    explicit Decoder(const Bytes& buffer) : buf_(buffer) {}

    bool init()
    {
        if (buf_.size() < sizeof(kMagic) + kTrailerSize ||
            !std::equal(std::begin(kMagic), std::end(kMagic), buf_.begin()))
        {
            SPDLOG_ERROR("[airplay] plist decode: bad magic or too short ({} bytes)", buf_.size());
            return false;
        }

        const size_t trailer = buf_.size() - kTrailerSize;
        offset_size_ = buf_[trailer + 6];
        ref_size_ = buf_[trailer + 7];
        num_objects_ = readSized(trailer + 8, 8);
        top_object_ = readSized(trailer + 16, 8);
        offset_table_ = readSized(trailer + 24, 8);

        if (offset_size_ == 0 || offset_size_ > 8 || ref_size_ == 0 || ref_size_ > 8 || num_objects_ == 0 ||
            top_object_ >= num_objects_ || offset_table_ > buf_.size() ||
            num_objects_ > (buf_.size() - offset_table_) / offset_size_)
        {
            SPDLOG_ERROR(
                "[airplay] plist decode: bad trailer (objects {}, top {}, table {}, offset size {}, ref size {}, "
                "buffer {} bytes)",
                num_objects_, top_object_, offset_table_, offset_size_, ref_size_, buf_.size());
            return false;
        }

        offsets_.reserve(num_objects_);
        for (size_t i = 0; i < num_objects_; ++i)
        {
            const size_t offset = readSized(offset_table_ + i * offset_size_, offset_size_);
            if (offset >= buf_.size())
            {
                SPDLOG_ERROR("[airplay] plist decode: object {} offset {} past end ({} bytes)", i, offset,
                             buf_.size());
                return false;
            }
            offsets_.push_back(offset);
        }
        return true;
    }

    std::optional<Value> readTop() { return readObject(top_object_, 0); }

private:
    uint64_t readSized(size_t base, size_t size) const
    {
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i)
        {
            if (base + i >= buf_.size())
            {
                return 0;
            }
            value = (value << 8) | buf_[base + i];
        }
        return value;
    }

    bool have(size_t position, size_t length) const
    {
        return position <= buf_.size() && length <= buf_.size() - position;
    }

    std::optional<size_t> readCount(size_t& p, uint8_t nib)
    {
        if (nib != 0x0f)
        {
            return static_cast<size_t>(nib);
        }
        if (!have(p, 1))
        {
            return std::nullopt;
        }
        const uint8_t size_marker = buf_[p++];
        if ((size_marker >> 4) != 0x1)
        {
            SPDLOG_ERROR("[airplay] plist decode: bad extended count marker 0x{:02x}", size_marker);
            return std::nullopt;
        }
        const size_t int_bytes = static_cast<size_t>(1) << (size_marker & 0x0f);
        if (int_bytes > 8 || !have(p, int_bytes))
        {
            SPDLOG_ERROR("[airplay] plist decode: bad extended count size {}", int_bytes);
            return std::nullopt;
        }
        const uint64_t count = readSized(p, int_bytes);
        p += int_bytes;
        if (count > buf_.size())
        {
            SPDLOG_ERROR("[airplay] plist decode: count {} exceeds buffer ({} bytes)", count, buf_.size());
            return std::nullopt;
        }
        return static_cast<size_t>(count);
    }

    std::optional<Value> readObject(size_t index, size_t depth)
    {
        if (depth > kMaxDepth)
        {
            SPDLOG_ERROR("[airplay] plist decode: nesting deeper than {} levels", kMaxDepth);
            return std::nullopt;
        }
        if (++visits_ > kMaxVisits)
        {
            SPDLOG_ERROR("[airplay] plist decode: object budget exhausted (possible reference cycle)");
            return std::nullopt;
        }
        if (index >= offsets_.size())
        {
            SPDLOG_ERROR("[airplay] plist decode: object reference {} out of range ({} objects)", index,
                         offsets_.size());
            return std::nullopt;
        }

        size_t p = offsets_[index];
        if (!have(p, 1))
        {
            return std::nullopt;
        }
        const uint8_t marker = buf_[p++];
        const uint8_t type = marker >> 4;
        const uint8_t nib = marker & 0x0f;

        switch (type)
        {
            case 0x0:
                if (nib == 0x00)
                {
                    return Value();
                }
                if (nib == 0x08)
                {
                    return Value::boolean(false);
                }
                if (nib == 0x09)
                {
                    return Value::boolean(true);
                }
                SPDLOG_ERROR("[airplay] plist decode: unsupported primitive 0x0{:x}", nib);
                return std::nullopt;

            case 0x1:
            {
                const size_t nbytes = static_cast<size_t>(1) << nib;
                if (nbytes > 16 || !have(p, nbytes))
                {
                    SPDLOG_ERROR("[airplay] plist decode: bad integer width {}", nbytes);
                    return std::nullopt;
                }
                if (nbytes == 16)
                {
                    // 128-bit integers only ever carry a UInt64 in practice.
                    if (readSized(p, 8) != 0)
                    {
                        SPDLOG_ERROR("[airplay] plist decode: 128-bit integer too large");
                        return std::nullopt;
                    }
                    return Value::integer(static_cast<int64_t>(readSized(p + 8, 8)));
                }
                const uint64_t raw = readSized(p, nbytes);
                // Apple reads 1/2/4-byte integers unsigned and 8-byte signed.
                return Value::integer(static_cast<int64_t>(raw));
            }

            case 0x2:
            {
                const size_t nbytes = static_cast<size_t>(1) << nib;
                if (!have(p, nbytes))
                {
                    return std::nullopt;
                }
                if (nbytes == 4)
                {
                    return Value::real(static_cast<double>(bitsToFloat(static_cast<uint32_t>(readSized(p, 4)))));
                }
                if (nbytes == 8)
                {
                    return Value::real(bitsToDouble(readSized(p, 8)));
                }
                SPDLOG_ERROR("[airplay] plist decode: unsupported real size {}", nbytes);
                return std::nullopt;
            }

            case 0x3:
            {
                if (nib != 0x3 || !have(p, 8))
                {
                    SPDLOG_ERROR("[airplay] plist decode: bad date marker 0x{:02x}", marker);
                    return std::nullopt;
                }
                return Value::date(bitsToDouble(readSized(p, 8)));
            }

            case 0x4:
            {
                const auto count = readCount(p, nib);
                if (!count || !have(p, *count))
                {
                    SPDLOG_ERROR("[airplay] plist decode: truncated data object");
                    return std::nullopt;
                }
                return Value::data(Bytes(buf_.begin() + static_cast<ptrdiff_t>(p),
                                         buf_.begin() + static_cast<ptrdiff_t>(p + *count)));
            }

            case 0x5:
            {
                const auto count = readCount(p, nib);
                if (!count || !have(p, *count))
                {
                    SPDLOG_ERROR("[airplay] plist decode: truncated ASCII string");
                    return std::nullopt;
                }
                return Value::string(std::string(reinterpret_cast<const char*>(buf_.data() + p), *count));
            }

            case 0x6:
            {
                const auto count = readCount(p, nib);
                if (!count || *count > buf_.size() / 2 || !have(p, *count * 2))
                {
                    SPDLOG_ERROR("[airplay] plist decode: truncated UTF-16 string");
                    return std::nullopt;
                }
                return Value::string(utf16BeToUtf8(buf_.data() + p, *count));
            }

            case 0xa:
            case 0xc:  // set, decoded as an array
            {
                const auto count = readCount(p, nib);
                if (!count || *count > buf_.size() / ref_size_ || !have(p, *count * ref_size_))
                {
                    SPDLOG_ERROR("[airplay] plist decode: truncated array");
                    return std::nullopt;
                }
                Value out = Value::array();
                for (size_t i = 0; i < *count; ++i)
                {
                    auto child = readObject(readSized(p + i * ref_size_, ref_size_), depth + 1);
                    if (!child)
                    {
                        return std::nullopt;
                    }
                    out.push(std::move(*child));
                }
                return out;
            }

            case 0xd:
            {
                const auto count = readCount(p, nib);
                if (!count || *count > buf_.size() / (2 * ref_size_) || !have(p, *count * 2 * ref_size_))
                {
                    SPDLOG_ERROR("[airplay] plist decode: truncated dict");
                    return std::nullopt;
                }
                Value out = Value::dict();
                for (size_t i = 0; i < *count; ++i)
                {
                    auto key = readObject(readSized(p + i * ref_size_, ref_size_), depth + 1);
                    auto value = readObject(readSized(p + (*count + i) * ref_size_, ref_size_), depth + 1);
                    if (!key || !value || !key->isString())
                    {
                        SPDLOG_ERROR("[airplay] plist decode: bad dict entry {}", i);
                        return std::nullopt;
                    }
                    out.set(key->asString(), std::move(*value));
                }
                return out;
            }

            default:
                SPDLOG_ERROR("[airplay] plist decode: unsupported object type 0x{:x}", type);
                return std::nullopt;
        }
    }

    const Bytes& buf_;
    std::vector<size_t> offsets_;
    size_t offset_size_ = 0;
    size_t ref_size_ = 0;
    size_t num_objects_ = 0;
    size_t top_object_ = 0;
    size_t offset_table_ = 0;
    size_t visits_ = 0;
};

}  // namespace

std::optional<Value> decode(const Bytes& buffer)
{
    Decoder decoder(buffer);
    if (!decoder.init())
    {
        return std::nullopt;
    }
    return decoder.readTop();
}

}  // namespace airplay::plist
