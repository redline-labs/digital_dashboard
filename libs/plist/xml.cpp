// SPDX-License-Identifier: GPL-3.0-or-later
//
// XML property lists. The binary format (binary.cpp) is what CarPlay's RTSP
// channel speaks; this is what the usbmux socket and lockdown speak, so both
// share the Value model rather than one being converted into the other.
#include "plist/xml.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace plist
{

namespace
{

// --- base64, for <data> ------------------------------------------------------

constexpr char kB64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 0-63 for an alphabet character, -1 for anything else. Whitespace lands in the
// "anything else" bucket and is skipped by the decoder, which is what lets
// wrapped <data> blocks parse.
int b64Value(char c)
{
    const char* p = std::strchr(kB64Alphabet, c);
    // strchr finds the NUL terminator too; '\0' is not a base64 digit.
    return (p != nullptr && c != '\0') ? static_cast<int>(p - kB64Alphabet) : -1;
}

// Wrapped at `columns` characters per line with `indent` before each, matching
// libplist's layout so encoder output can be compared byte for byte.
std::string b64Encode(const Bytes& in, const std::string& indent, size_t columns)
{
    std::string raw;
    raw.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3)
    {
        const uint32_t b0 = in[i];
        const uint32_t b1 = (i + 1 < in.size()) ? in[i + 1] : 0;
        const uint32_t b2 = (i + 2 < in.size()) ? in[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        raw += kB64Alphabet[(triple >> 18) & 0x3F];
        raw += kB64Alphabet[(triple >> 12) & 0x3F];
        raw += (i + 1 < in.size()) ? kB64Alphabet[(triple >> 6) & 0x3F] : '=';
        raw += (i + 2 < in.size()) ? kB64Alphabet[triple & 0x3F] : '=';
    }

    std::string out;
    for (size_t i = 0; i < raw.size(); i += columns)
    {
        out += indent;
        out += raw.substr(i, columns);
        out += '\n';
    }
    return out;
}

// Returns false on a character that is neither an alphabet digit, '=' nor
// whitespace -- that means the document is not what it claims to be.
bool b64Decode(std::string_view in, Bytes& out)
{
    uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : in)
    {
        if (c == '=')
        {
            break;
        }
        const int v = b64Value(c);
        if (v < 0)
        {
            if (std::isspace(static_cast<unsigned char>(c)) != 0)
            {
                continue;
            }
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

// --- text escaping -----------------------------------------------------------

void appendEscaped(std::string& out, std::string_view text)
{
    for (const char c : text)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c; break;
        }
    }
}

// Resolves the five predefined entities plus numeric character references,
// re-encoding the latter as UTF-8. An unrecognised "&...;" is left verbatim,
// which is what every lenient XML reader does and keeps a stray '&' in a device
// name from failing the whole parse.
std::string unescape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '&')
        {
            out += text[i];
            continue;
        }
        const size_t end = text.find(';', i + 1);
        if (end == std::string_view::npos || end - i > 12)
        {
            out += text[i];
            continue;
        }
        const std::string_view entity = text.substr(i + 1, end - i - 1);
        if (entity == "amp") { out += '&'; }
        else if (entity == "lt") { out += '<'; }
        else if (entity == "gt") { out += '>'; }
        else if (entity == "quot") { out += '"'; }
        else if (entity == "apos") { out += '\''; }
        else if (entity.size() > 1 && entity[0] == '#')
        {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            const std::string digits(entity.substr(hex ? 2 : 1));
            char* stop = nullptr;
            const unsigned long code = std::strtoul(digits.c_str(), &stop, hex ? 16 : 10);
            if (digits.empty() || stop == nullptr || *stop != '\0' || code > 0x10FFFF)
            {
                out += text[i];
                continue;
            }
            // Inline UTF-8 encode; the code point came from the document, so the
            // four-byte case is reachable.
            if (code < 0x80)
            {
                out += static_cast<char>(code);
            }
            else if (code < 0x800)
            {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }
        else
        {
            out += text[i];
            continue;
        }
        i = end;
    }
    return out;
}

// --- dates -------------------------------------------------------------------

// Property list dates are stored as seconds from 2001-01-01T00:00:00Z but
// written as ISO 8601 against the Unix epoch.
constexpr int64_t kAppleEpochInUnix = 978307200;

// Fixed-width zero-padded field. std::tm carries no range invariant the
// compiler can see, so "%02d" into a two-character slot reads as a possible
// truncation; doing the digits by hand makes the width a property of the code.
std::string padded(int value, size_t width)
{
    std::string digits = std::to_string(value < 0 ? -value : value);
    if (digits.size() < width)
    {
        digits.insert(0, width - digits.size(), '0');
    }
    return (value < 0 ? "-" : "") + digits;
}

std::string formatDate(double seconds_since_2001)
{
    const std::time_t unix_seconds =
        static_cast<std::time_t>(std::llround(seconds_since_2001) + kAppleEpochInUnix);
    std::tm utc{};
    gmtime_r(&unix_seconds, &utc);
    return padded(utc.tm_year + 1900, 4) + "-" + padded(utc.tm_mon + 1, 2) + "-" +
           padded(utc.tm_mday, 2) + "T" + padded(utc.tm_hour, 2) + ":" + padded(utc.tm_min, 2) +
           ":" + padded(utc.tm_sec, 2) + "Z";
}

bool parseDate(const std::string& text, double& out)
{
    std::tm utc{};
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%dZ", &utc.tm_year, &utc.tm_mon, &utc.tm_mday,
                    &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6)
    {
        return false;
    }
    utc.tm_year -= 1900;
    utc.tm_mon -= 1;
    // timegm rather than mktime: the fields are UTC and must not be run through
    // the host's local timezone.
    const std::time_t unix_seconds = timegm(&utc);
    if (unix_seconds == static_cast<std::time_t>(-1))
    {
        return false;
    }
    out = static_cast<double>(static_cast<int64_t>(unix_seconds) - kAppleEpochInUnix);
    return true;
}

// --- encoding ----------------------------------------------------------------

void encodeValue(std::string& out, const Value& value, int depth)
{
    const std::string indent(static_cast<size_t>(depth), '\t');
    switch (value.type())
    {
        case Value::Type::Null:
            // No XML element models "absent". Writing <string/> keeps the
            // document well formed and round-trips to an empty string, which is
            // the closest thing the format has.
            out += indent + "<string></string>\n";
            break;
        case Value::Type::Bool:
            out += indent + (value.asBool() ? "<true/>\n" : "<false/>\n");
            break;
        case Value::Type::Integer:
        {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%" PRId64, value.asInteger());
            out += indent + "<integer>" + buf + "</integer>\n";
            break;
        }
        case Value::Type::Real:
        {
            char buf[40];
            // 17 significant digits is the round-trip guarantee for a double.
            std::snprintf(buf, sizeof(buf), "%.17g", value.asReal());
            out += indent + "<real>" + buf + "</real>\n";
            break;
        }
        case Value::Type::String:
            out += indent + "<string>";
            appendEscaped(out, value.asString());
            out += "</string>\n";
            break;
        case Value::Type::Data:
        {
            const Bytes& bytes = value.asData();
            if (bytes.empty())
            {
                out += indent + "<data></data>\n";
                break;
            }
            out += indent + "<data>\n";
            out += b64Encode(bytes, indent, 60);
            out += indent + "</data>\n";
            break;
        }
        case Value::Type::Date:
            out += indent + "<date>" + formatDate(value.asDate()) + "</date>\n";
            break;
        case Value::Type::Array:
            if (value.size() == 0)
            {
                out += indent + "<array/>\n";
                break;
            }
            out += indent + "<array>\n";
            for (size_t i = 0; i < value.size(); ++i)
            {
                encodeValue(out, value.at(i), depth + 1);
            }
            out += indent + "</array>\n";
            break;
        case Value::Type::Dict:
            if (value.size() == 0)
            {
                out += indent + "<dict/>\n";
                break;
            }
            out += indent + "<dict>\n";
            for (size_t i = 0; i < value.size(); ++i)
            {
                out += indent + "\t<key>";
                appendEscaped(out, value.keys()[i]);
                out += "</key>\n";
                encodeValue(out, value.valueAt(i), depth + 1);
            }
            out += indent + "</dict>\n";
            break;
    }
}

// --- parsing -----------------------------------------------------------------

class Parser
{
  public:
    explicit Parser(std::string_view in) : in_(in) {}

    std::optional<Value> run()
    {
        skipProlog();
        if (!expectOpen("plist"))
        {
            return std::nullopt;
        }
        // <plist version="1.0"/> -- legal, and empty.
        if (last_was_self_closing_)
        {
            return Value();
        }
        auto root = parseValue();
        if (!root)
        {
            return std::nullopt;
        }
        skipIgnorable();
        if (!expectClose("plist"))
        {
            return std::nullopt;
        }
        return root;
    }

  private:
    // Whitespace, comments, processing instructions and the DOCTYPE -- anything
    // that can legally sit between elements and carries no plist meaning.
    void skipIgnorable()
    {
        for (;;)
        {
            while (pos_ < in_.size() && std::isspace(static_cast<unsigned char>(in_[pos_])) != 0)
            {
                ++pos_;
            }
            if (starts("<?"))
            {
                const size_t end = in_.find("?>", pos_);
                pos_ = (end == std::string_view::npos) ? in_.size() : end + 2;
            }
            else if (starts("<!--"))
            {
                const size_t end = in_.find("-->", pos_);
                pos_ = (end == std::string_view::npos) ? in_.size() : end + 3;
            }
            else if (starts("<!"))
            {
                skipDoctype();
            }
            else
            {
                return;
            }
        }
    }

    // A DOCTYPE may carry an internal subset in brackets, which can itself
    // contain '>' characters, so scanning to the first '>' is not enough.
    void skipDoctype()
    {
        pos_ += 2;
        bool in_subset = false;
        while (pos_ < in_.size())
        {
            const char c = in_[pos_++];
            if (c == '[')
            {
                in_subset = true;
            }
            else if (c == ']')
            {
                in_subset = false;
            }
            else if (c == '>' && !in_subset)
            {
                return;
            }
        }
    }

    void skipProlog() { skipIgnorable(); }

    bool starts(std::string_view prefix) const { return in_.compare(pos_, prefix.size(), prefix) == 0; }

    // Reads "<name ...>" or "<name .../>", leaving pos_ past the '>'.
    bool expectOpen(std::string_view name)
    {
        skipIgnorable();
        if (pos_ >= in_.size() || in_[pos_] != '<')
        {
            return false;
        }
        const size_t end = in_.find('>', pos_);
        if (end == std::string_view::npos)
        {
            return false;
        }
        std::string_view tag = in_.substr(pos_ + 1, end - pos_ - 1);
        last_was_self_closing_ = !tag.empty() && tag.back() == '/';
        if (last_was_self_closing_)
        {
            tag.remove_suffix(1);
        }
        // Attributes are not modelled; only the element name matters.
        const size_t name_end = tag.find_first_of(" \t\r\n");
        if (name_end != std::string_view::npos)
        {
            tag = tag.substr(0, name_end);
        }
        if (tag != name)
        {
            return false;
        }
        pos_ = end + 1;
        return true;
    }

    bool expectClose(std::string_view name)
    {
        skipIgnorable();
        const std::string closing = "</" + std::string(name);
        if (!starts(closing))
        {
            return false;
        }
        const size_t end = in_.find('>', pos_);
        if (end == std::string_view::npos)
        {
            return false;
        }
        pos_ = end + 1;
        return true;
    }

    // The element name at pos_, without consuming it.
    std::string peekTagName()
    {
        skipIgnorable();
        if (pos_ >= in_.size() || in_[pos_] != '<')
        {
            return {};
        }
        size_t i = pos_ + 1;
        if (i < in_.size() && in_[i] == '/')
        {
            ++i;
        }
        const size_t start = i;
        while (i < in_.size() && (std::isalnum(static_cast<unsigned char>(in_[i])) != 0))
        {
            ++i;
        }
        return std::string(in_.substr(start, i - start));
    }

    // Raw text up to the next '<'.
    std::string readText()
    {
        const size_t end = in_.find('<', pos_);
        const size_t stop = (end == std::string_view::npos) ? in_.size() : end;
        std::string text(in_.substr(pos_, stop - pos_));
        pos_ = stop;
        return text;
    }

    std::optional<Value> parseValue()
    {
        if (++depth_ > kMaxDepth)
        {
            return std::nullopt;
        }
        auto result = parseValueInner();
        --depth_;
        return result;
    }

    std::optional<Value> parseValueInner()
    {
        const std::string tag = peekTagName();
        if (tag.empty())
        {
            return std::nullopt;
        }

        if (tag == "true" || tag == "false")
        {
            if (!expectOpen(tag))
            {
                return std::nullopt;
            }
            if (!last_was_self_closing_ && !expectClose(tag))
            {
                return std::nullopt;
            }
            return Value::boolean(tag == "true");
        }

        if (tag == "dict" || tag == "array")
        {
            const bool is_dict = tag == "dict";
            if (!expectOpen(tag))
            {
                return std::nullopt;
            }
            Value out = is_dict ? Value::dict() : Value::array();
            if (last_was_self_closing_)
            {
                return out;
            }
            for (;;)
            {
                skipIgnorable();
                if (starts("</"))
                {
                    break;
                }
                if (pos_ >= in_.size())
                {
                    return std::nullopt;
                }
                if (is_dict)
                {
                    if (!expectOpen("key"))
                    {
                        return std::nullopt;
                    }
                    // <key/> is degenerate but well formed; treat it as "".
                    std::string key;
                    if (!last_was_self_closing_)
                    {
                        key = unescape(readText());
                        if (!expectClose("key"))
                        {
                            return std::nullopt;
                        }
                    }
                    auto child = parseValue();
                    if (!child)
                    {
                        return std::nullopt;
                    }
                    out.set(std::move(key), std::move(*child));
                }
                else
                {
                    auto child = parseValue();
                    if (!child)
                    {
                        return std::nullopt;
                    }
                    out.push(std::move(*child));
                }
            }
            if (!expectClose(tag))
            {
                return std::nullopt;
            }
            return out;
        }

        if (tag == "string" || tag == "integer" || tag == "real" || tag == "data" || tag == "date")
        {
            if (!expectOpen(tag))
            {
                return std::nullopt;
            }
            std::string text;
            if (!last_was_self_closing_)
            {
                text = readText();
                if (!expectClose(tag))
                {
                    return std::nullopt;
                }
            }

            if (tag == "string")
            {
                return Value::string(unescape(text));
            }
            if (tag == "data")
            {
                Bytes bytes;
                if (!b64Decode(text, bytes))
                {
                    return std::nullopt;
                }
                return Value::data(std::move(bytes));
            }

            const std::string trimmed = trim(text);
            if (tag == "integer")
            {
                char* stop = nullptr;
                // strtoll rather than strtoull: plist integers are signed, and
                // libplist writes the unsigned 64-bit range out as negatives.
                const long long v = std::strtoll(trimmed.c_str(), &stop, 10);
                if (trimmed.empty() || stop == nullptr || *stop != '\0')
                {
                    return std::nullopt;
                }
                return Value::integer(static_cast<int64_t>(v));
            }
            if (tag == "real")
            {
                char* stop = nullptr;
                const double v = std::strtod(trimmed.c_str(), &stop);
                if (trimmed.empty() || stop == nullptr || *stop != '\0')
                {
                    return std::nullopt;
                }
                return Value::real(v);
            }
            double seconds = 0.0;
            if (!parseDate(trimmed, seconds))
            {
                return std::nullopt;
            }
            return Value::date(seconds);
        }

        // Anything else is outside the plist vocabulary.
        return std::nullopt;
    }

    static std::string trim(const std::string& s)
    {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    // Bounds recursion so a hostile or corrupt document cannot overflow the
    // stack. Real plists from a phone nest a handful of levels at most.
    static constexpr int kMaxDepth = 64;

    std::string_view in_;
    size_t pos_ = 0;
    int depth_ = 0;
    bool last_was_self_closing_ = false;
};

}  // namespace

std::string encodeXml(const Value& root)
{
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n";
    encodeValue(out, root, 0);
    out += "</plist>\n";
    return out;
}

std::optional<Value> decodeXml(std::string_view xml)
{
    Parser parser(xml);
    return parser.run();
}

}  // namespace plist
