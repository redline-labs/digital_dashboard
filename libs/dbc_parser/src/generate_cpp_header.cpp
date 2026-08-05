#include "dbc_parser/generate_h.h"

#include "dbc_parser/dbc_parser.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace dbc_codegen
{
namespace
{

// Text from a DBC ends up inside a C++ string literal. The lexer already
// removed the file's own escaping, so a comment containing a quote arrives here
// as a bare quote and would close the literal early -- a vendor-supplied file
// deciding what our build compiles.
std::string stringLiteral(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');

    for (char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;

        case '\\':
            out += "\\\\";
            break;

        case '\n':
            out += "\\n";
            break;

        case '\r':
            out += "\\r";
            break;

        case '\t':
            out += "\\t";
            break;

        default:
            if (static_cast<unsigned char>(c) < 0x20u)
            {
                out += fmt::format("\\x{:02x}", static_cast<unsigned char>(c));
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }

    out.push_back('"');
    return out;
}

// A double that round-trips exactly and is always spelled as a double, so
// `1` does not become an int literal in a constexpr double context.
std::string literal(double value)
{
    std::string text = fmt::format("{:.17g}", value);
    if (text.find_first_of(".eE") == std::string::npos)
    {
        text += ".0";
    }
    return text;
}

enum class DecodedType
{
    Enum,
    Double,
    Int64,
    UInt64,
};

bool isIntegral(double value)
{
    return std::isfinite(value) && (std::floor(value) == value);
}

// What C++ type a decoded signal is handed to callers as.
//
// The rule this replaced looked only at `scale` and `isSigned`, so an unsigned
// signal with a negative offset -- Motec's `(1,-40)` temperatures -- got an
// unsigned type and every reading below 40 was converted out of range. That is
// undefined behaviour, and in practice produced one junk constant for the whole
// cold half of the scale.
DecodedType decodedType(const dbc_parser::Signal &signal)
{
    // A value table becomes an enum only when raw and physical are the same
    // number. VAL_ maps *raw* values to names, so with a scale or offset in
    // play the enumerators would name unscaled values while the field held a
    // scaled one -- two different numbers wearing the same name.
    if (!signal.valueTable.empty() && (signal.scale == 1.0) && (signal.offset == 0.0) &&
        (signal.valueType == dbc_parser::SignalValueType::Integer))
    {
        return DecodedType::Enum;
    }

    if (signal.valueType != dbc_parser::SignalValueType::Integer)
    {
        return DecodedType::Double;
    }

    if (!isIntegral(signal.scale) || !isIntegral(signal.offset))
    {
        return DecodedType::Double;
    }

    // The physical value is raw * scale + offset. It can be negative because
    // the field is signed, because the offset is, or because the scale is.
    if (signal.isSigned || (signal.offset < 0.0) || (signal.scale < 0.0))
    {
        return DecodedType::Int64;
    }

    return DecodedType::UInt64;
}

std::string_view typeName(DecodedType type)
{
    switch (type)
    {
    case DecodedType::Enum:
        return "Values";

    case DecodedType::Double:
        return "double";

    case DecodedType::Int64:
        return "int64_t";

    case DecodedType::UInt64:
        return "uint64_t";
    }

    return "double";
}

// A value table only becomes an enum when raw and physical are the same
// number. With a scale or offset in play the enumerators would be raw values
// while the field held a scaled one, and the two would silently disagree.
bool emitsEnum(const dbc_parser::Signal &signal)
{
    return decodedType(signal) == DecodedType::Enum;
}

// Enumerator names come from free text in the DBC. Anything that is not a
// valid identifier has to be repaired, and the repair has to be collision-free
// or the generated enum simply will not compile.
std::vector<std::string> enumeratorNames(const dbc_parser::Signal &signal)
{
    std::vector<std::string> names;
    names.reserve(signal.valueTable.size());

    for (const auto &mapping : signal.valueTable)
    {
        std::string name;
        name.reserve(mapping.description.size());
        for (char c : mapping.description)
        {
            name.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        }

        // Trim the underscores a leading or trailing separator leaves behind,
        // so "Output On " reads as Output_On rather than Output_On_.
        size_t begin = name.find_first_not_of('_');
        size_t end = name.find_last_not_of('_');
        name = (begin == std::string::npos) ? std::string{} : name.substr(begin, end - begin + 1);

        if (name.empty())
        {
            name = "Value";
        }

        if (std::isdigit(static_cast<unsigned char>(name.front())))
        {
            name.insert(name.begin(), '_');
        }

        // A description like "class" sanitises to a perfectly good identifier
        // that still cannot be an enumerator. Trailing underscore rather than
        // leading, because a leading one before an uppercase letter is
        // reserved to the implementation.
        while (!dbc_parser::isUsableIdentifier(name))
        {
            name.push_back('_');
        }

        names.push_back(std::move(name));
    }

    // Only names that actually collide get disambiguated. The version this
    // replaced compared every entry against itself, so the self-match always
    // fired and *every* enumerator was suffixed with its own raw value --
    // Output_Off_0, Output_On_1 -- whether or not anything clashed.
    std::set<std::string> taken;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (taken.insert(names[i]).second)
        {
            continue;
        }

        std::string candidate = names[i] + "_" + std::to_string(signal.valueTable[i].rawValue);
        // A raw value is unique within a table, but a name ending in that same
        // digit run could still land on top of one.
        size_t attempt = 2;
        while (!taken.insert(candidate).second)
        {
            candidate = names[i] + "_" + std::to_string(signal.valueTable[i].rawValue) + "_" +
                        std::to_string(attempt);
            attempt += 1;
        }
        names[i] = candidate;
    }

    return names;
}

void generateSignalTraits(const dbc_parser::Signal &signal, std::ostream &out)
{
    const DecodedType type = decodedType(signal);

    fmt::print(out, "    struct sig_{}_t\n", signal.name);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        static constexpr std::string_view name = {};\n", stringLiteral(signal.name));
    fmt::print(out, "        static constexpr std::string_view comment = {};\n", stringLiteral(signal.comment));
    fmt::print(out, "        static constexpr std::string_view unit = {};\n", stringLiteral(signal.unit));
    fmt::print(out, "\n");
    fmt::print(out, "        static constexpr uint32_t start_bit = {}u;\n", signal.startBit);
    fmt::print(out, "        static constexpr uint32_t length = {}u;\n", signal.length);
    fmt::print(out, "        static constexpr bool little_endian = {};\n", signal.littleEndian);
    fmt::print(out, "        static constexpr bool is_signed = {};\n", signal.isSigned);
    fmt::print(out, "\n");
    fmt::print(out, "        static constexpr bool is_multiplex = {};\n", signal.isMultiplex);
    fmt::print(out, "        static constexpr bool is_multiplexor = {};\n", signal.isMultiplexor);
    fmt::print(out, "        static constexpr uint32_t multiplexed_group_idx = {}u;\n",
               signal.multiplexedGroupIdx);
    fmt::print(out, "\n");

    // Scale and offset are emitted for every signal, including enumerated
    // ones. Leaving them off value-table signals meant a scaled enum lost its
    // scaling entirely, and callers had no way to notice.
    fmt::print(out, "        static constexpr double scale = {};\n", literal(signal.scale));
    fmt::print(out, "        static constexpr double offset = {};\n", literal(signal.offset));
    fmt::print(out, "        static constexpr double minimum = {};\n", literal(signal.minimum));
    fmt::print(out, "        static constexpr double maximum = {};\n", literal(signal.maximum));
    fmt::print(out, "\n");

    switch (signal.valueType)
    {
    case dbc_parser::SignalValueType::Integer:
        fmt::print(out, "        static constexpr raw_encoding encoding = raw_encoding::Integer;\n");
        break;

    case dbc_parser::SignalValueType::Float:
        fmt::print(out, "        static constexpr raw_encoding encoding = raw_encoding::Float;\n");
        break;

    case dbc_parser::SignalValueType::Double:
        fmt::print(out, "        static constexpr raw_encoding encoding = raw_encoding::Double;\n");
        break;
    }

    fmt::print(out, "        static constexpr bool has_value_table = {};\n", emitsEnum(signal));
    fmt::print(out, "\n");

    if (emitsEnum(signal))
    {
        const std::vector<std::string> names = enumeratorNames(signal);

        // int64_t underlying, because a raw value from the file can be wider
        // than the int an unscoped enum would default to.
        fmt::print(out, "        enum class Values : int64_t\n");
        fmt::print(out, "        {{\n");
        for (size_t i = 0; i < names.size(); ++i)
        {
            fmt::print(out, "            {} = {},\n", names[i], signal.valueTable[i].rawValue);
        }
        fmt::print(out, "        }};\n");
        fmt::print(out, "\n");
    }

    fmt::print(out, "        // The type decoded values of this signal are handed over as.\n");
    fmt::print(out, "        using Type = {};\n", typeName(type));
    fmt::print(out, "\n");
    fmt::print(out, "        static constexpr std::array<std::string_view, {}> receivers =\n",
               signal.receivers.size());
    fmt::print(out, "        {{\n");
    for (const auto &receiver : signal.receivers)
    {
        fmt::print(out, "            {},\n", stringLiteral(receiver));
    }
    fmt::print(out, "        }};\n");
    fmt::print(out, "    }};\n");
    fmt::print(out, "\n");
}

void generateBitHelpers(std::ostream &out)
{
    fmt::print(out, "    // Pull Sig::length bits out of the frame into a right aligned word.\n");
    fmt::print(out, "    template <typename Sig>\n");
    fmt::print(out, "    static constexpr uint64_t extract_bits(std::span<const uint8_t> data)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        uint64_t raw_u = 0;\n");
    fmt::print(out, "        if constexpr (Sig::little_endian)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t abs_bit = Sig::start_bit + i;\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((data[abs_bit / 8u] >> (abs_bit % 8u)) & 0x1u);\n");
    fmt::print(out, "                raw_u |= (static_cast<uint64_t>(bit) << i);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            // Motorola order: walk down within a byte, then jump to the\n");
    fmt::print(out, "            // top of the next one.\n");
    fmt::print(out, "            uint32_t abs_bit = Sig::start_bit;\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((data[abs_bit / 8u] >> (abs_bit % 8u)) & 0x1u);\n");
    fmt::print(out, "                raw_u = (raw_u << 1) | static_cast<uint64_t>(bit);\n");
    fmt::print(out, "                if ((abs_bit % 8u) == 0u) abs_bit += 15u; else abs_bit -= 1u;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        return raw_u;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    template <typename Sig>\n");
    fmt::print(out, "    static constexpr void insert_bits(std::span<uint8_t> buf, uint64_t raw_u)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        if constexpr (Sig::little_endian)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t abs_bit = Sig::start_bit + i;\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((raw_u >> i) & 0x1u);\n");
    fmt::print(out, "                set_bit(buf, abs_bit, bit);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            uint32_t abs_bit = Sig::start_bit;\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((raw_u >> (Sig::length - 1u - i)) & 0x1u);\n");
    fmt::print(out, "                set_bit(buf, abs_bit, bit);\n");
    fmt::print(out, "                if ((abs_bit % 8u) == 0u) abs_bit += 15u; else abs_bit -= 1u;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    static constexpr void set_bit(std::span<uint8_t> buf, uint32_t abs_bit, uint8_t bit)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        uint8_t& byte = buf[abs_bit / 8u];\n");
    fmt::print(out, "        const uint8_t mask = static_cast<uint8_t>(1u << (abs_bit % 8u));\n");
    fmt::print(out, "        byte = static_cast<uint8_t>((byte & static_cast<uint8_t>(~mask)) | (bit ? mask : 0u));\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Decode
    fmt::print(out, "    template <typename Sig>\n");
    fmt::print(out, "    static constexpr typename Sig::Type extract(std::span<const uint8_t> data)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        static_assert(Sig::length >= 1 && Sig::length <= 64);\n");
    fmt::print(out, "        const uint64_t raw_u = extract_bits<Sig>(data);\n");
    fmt::print(out, "\n");
    fmt::print(out, "        if constexpr (Sig::encoding == raw_encoding::Float)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            const float raw_f = std::bit_cast<float>(static_cast<uint32_t>(raw_u));\n");
    fmt::print(out, "            return static_cast<typename Sig::Type>(static_cast<double>(raw_f) * Sig::scale + Sig::offset);\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else if constexpr (Sig::encoding == raw_encoding::Double)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            const double raw_d = std::bit_cast<double>(raw_u);\n");
    fmt::print(out, "            return static_cast<typename Sig::Type>(raw_d * Sig::scale + Sig::offset);\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            int64_t raw = static_cast<int64_t>(raw_u);\n");
    fmt::print(out, "            if constexpr (Sig::is_signed && Sig::length < 64)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                if (((raw_u >> (Sig::length - 1u)) & 0x1u) != 0u)\n");
    fmt::print(out, "                {{\n");
    fmt::print(out, "                    raw |= (~0ll) << Sig::length;\n");
    fmt::print(out, "                }}\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "            if constexpr (Sig::has_value_table)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                return static_cast<typename Sig::Type>(raw);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "            else if constexpr (Sig::scale == 1.0 && Sig::offset == 0.0)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                // Identity scaling stays in the integer domain, so a 64 bit\n");
    fmt::print(out, "                // signal is not rounded by a trip through double.\n");
    fmt::print(out, "                return static_cast<typename Sig::Type>(raw);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "            else\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                return static_cast<typename Sig::Type>(static_cast<double>(raw) * Sig::scale + Sig::offset);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Encode
    fmt::print(out, "    template <typename Sig>\n");
    fmt::print(out, "    static constexpr uint64_t to_raw_u(typename Sig::Type value)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        constexpr uint64_t mask = (Sig::length == 64u) ? ~0ull : ((1ull << (Sig::length % 64u)) - 1ull);\n");
    fmt::print(out, "\n");
    fmt::print(out, "        if constexpr (Sig::has_value_table)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            return static_cast<uint64_t>(static_cast<int64_t>(value)) & mask;\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else if constexpr (Sig::encoding == raw_encoding::Float)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            const float raw_f = static_cast<float>((static_cast<double>(value) - Sig::offset) / Sig::scale);\n");
    fmt::print(out, "            return static_cast<uint64_t>(std::bit_cast<uint32_t>(raw_f));\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else if constexpr (Sig::encoding == raw_encoding::Double)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            const double raw_d = (static_cast<double>(value) - Sig::offset) / Sig::scale;\n");
    fmt::print(out, "            return std::bit_cast<uint64_t>(raw_d);\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else if constexpr (Sig::scale == 1.0 && Sig::offset == 0.0)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            // Identity scaling stays in the integer domain, mirroring\n");
    fmt::print(out, "            // extract(). A signal wider than 53 bits does not survive a\n");
    fmt::print(out, "            // trip through double: adding 0.5 to a value at the top of\n");
    fmt::print(out, "            // that range rounds it to the wrong integer outright.\n");
    fmt::print(out, "            if constexpr (Sig::is_signed)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                constexpr int64_t min_v = (Sig::length == 64u) ? std::numeric_limits<int64_t>::min() : -(1ll << ((Sig::length - 1u) % 63u));\n");
    fmt::print(out, "                constexpr int64_t max_v = (Sig::length == 64u) ? std::numeric_limits<int64_t>::max() : ((1ll << ((Sig::length - 1u) % 63u)) - 1ll);\n");
    fmt::print(out, "                int64_t raw = static_cast<int64_t>(value);\n");
    fmt::print(out, "                if (raw < min_v) raw = min_v;\n");
    fmt::print(out, "                if (raw > max_v) raw = max_v;\n");
    fmt::print(out, "                return static_cast<uint64_t>(raw) & mask;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "            else\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                uint64_t raw = static_cast<uint64_t>(value);\n");
    fmt::print(out, "                if (raw > mask) raw = mask;\n");
    fmt::print(out, "                return raw;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            const double raw_d = (static_cast<double>(value) - Sig::offset) / Sig::scale;\n");
    fmt::print(out, "\n");
    fmt::print(out, "            // Saturate before rounding, never after. Converting an out of\n");
    fmt::print(out, "            // range double to an integer is undefined, not clamping, and\n");
    fmt::print(out, "            // the bounds below are powers of two so they stay exact even\n");
    fmt::print(out, "            // for a field too wide for a double to enumerate.\n");
    fmt::print(out, "            if constexpr (Sig::is_signed)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                constexpr int64_t min_v = (Sig::length == 64u) ? std::numeric_limits<int64_t>::min() : -(1ll << ((Sig::length - 1u) % 63u));\n");
    fmt::print(out, "                constexpr int64_t max_v = (Sig::length == 64u) ? std::numeric_limits<int64_t>::max() : ((1ll << ((Sig::length - 1u) % 63u)) - 1ll);\n");
    fmt::print(out, "                constexpr double limit = two_pow(Sig::length - 1u);\n");
    fmt::print(out, "\n");
    fmt::print(out, "                // Written so a NaN takes the first branch rather than\n");
    fmt::print(out, "                // falling through to the conversion.\n");
    fmt::print(out, "                if (!(raw_d >= -limit))\n");
    fmt::print(out, "                {{\n");
    fmt::print(out, "                    return static_cast<uint64_t>(min_v) & mask;\n");
    fmt::print(out, "                }}\n");
    fmt::print(out, "                if (raw_d >= limit)\n");
    fmt::print(out, "                {{\n");
    fmt::print(out, "                    return static_cast<uint64_t>(max_v) & mask;\n");
    fmt::print(out, "                }}\n");
    fmt::print(out, "                return static_cast<uint64_t>(round_half_away_from_zero<int64_t>(raw_d)) & mask;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "            else\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                constexpr double limit = two_pow(Sig::length);\n");
    fmt::print(out, "\n");
    fmt::print(out, "                if (!(raw_d >= 0.0))\n");
    fmt::print(out, "                {{\n");
    fmt::print(out, "                    return 0ull;\n");
    fmt::print(out, "                }}\n");
    fmt::print(out, "                if (raw_d >= limit)\n");
    fmt::print(out, "                {{\n");
    fmt::print(out, "                    return mask;\n");
    fmt::print(out, "                }}\n");
    fmt::print(out, "                return round_half_away_from_zero<uint64_t>(raw_d) & mask;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
}

void generateMessageHeader(const dbc_parser::Message &message, const std::string &base,
                           std::ostream &out)
{
    const dbc_parser::Signal *muxSignal = message.multiplexor();

    std::set<uint32_t> muxGroups;
    for (const auto &signal : message.signals)
    {
        if (signal.isMultiplex)
        {
            muxGroups.insert(signal.multiplexedGroupIdx);
        }
    }

    // The parser rejects a message that has multiplexed signals without a
    // multiplexor, so this cannot be null here -- but it used to be
    // dereferenced unconditionally and take the generator down mid-write.
    const bool multiplexed = message.isMultiplexed && (muxSignal != nullptr);
    const uint32_t startMuxGroup = muxGroups.empty() ? 0u : *muxGroups.begin();

    std::string guard = base + "_" + message.name + "_H_";
    std::transform(guard.begin(), guard.end(), guard.begin(), ::toupper);

    fmt::print(out, "#ifndef {}\n", guard);
    fmt::print(out, "#define {}\n\n", guard);
    fmt::print(out, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(out, "#include <array>\n");
    fmt::print(out, "#include <bit>\n");
    fmt::print(out, "#include <cmath>\n");
    fmt::print(out, "#include <cstdint>\n");
    fmt::print(out, "#include <limits>\n");
    fmt::print(out, "#include <span>\n");
    fmt::print(out, "#include <string_view>\n");
    fmt::print(out, "\n");
    fmt::print(out, "#include \"{}_common.h\"\n", base);
    fmt::print(out, "\n");
    fmt::print(out, "namespace {}\n", base);
    fmt::print(out, "{{\n");
    fmt::print(out, "\n");
    fmt::print(out, "struct {}_t\n", message.name);
    fmt::print(out, "{{\n");
    fmt::print(out, "    static constexpr std::string_view name = {};\n", stringLiteral(message.name));
    fmt::print(out, "    static constexpr uint32_t id = 0x{:X}u;\n", message.id);
    fmt::print(out, "    static constexpr bool is_extended = {};\n", message.isExtended);
    fmt::print(out, "    static constexpr uint8_t dlc = {}u;\n", message.dlc);
    fmt::print(out, "    static constexpr std::string_view transmitter = {};\n",
               stringLiteral(message.transmitter));
    fmt::print(out, "    static constexpr std::string_view comment = {};\n", stringLiteral(message.comment));
    fmt::print(out, "\n");
    fmt::print(out, "    static constexpr size_t signal_count = {}u;\n", message.signals.size());
    fmt::print(out, "    static constexpr bool is_multiplexed = {};\n", multiplexed);

    if (multiplexed)
    {
        fmt::print(out, "    static constexpr std::string_view multiplexor_name = {};\n",
                   stringLiteral(muxSignal->name));
        fmt::print(out, "    static constexpr std::array<uint32_t, {}> multiplexor_group_indexes = {{{}}};\n",
                   muxGroups.size(), fmt::join(muxGroups, ", "));
        fmt::print(out, "    static constexpr uint32_t start_mux_group_index = {}u;\n", startMuxGroup);
    }

    fmt::print(out, "\n");
    fmt::print(out, "    static constexpr std::array<std::string_view, {}u> signal_names =\n",
               message.signals.size());
    fmt::print(out, "    {{\n");
    for (const auto &signal : message.signals)
    {
        fmt::print(out, "        {},\n", stringLiteral(signal.name));
    }
    fmt::print(out, "    }};\n");
    fmt::print(out, "\n");

    for (const auto &signal : message.signals)
    {
        generateSignalTraits(signal, out);
    }

    for (const auto &signal : message.signals)
    {
        fmt::print(out, "    sig_{}_t::Type {}{{}};\n", signal.name, signal.name);
    }
    fmt::print(out, "\n");

    if (multiplexed)
    {
        fmt::print(out, "    // One flag per multiplex group, so a caller can tell a complete\n");
        fmt::print(out, "    // batch from a half filled struct.\n");
        for (const auto &group : muxGroups)
        {
            fmt::print(out, "    bool seen_mux_{}{{false}};\n", group);
        }
        fmt::print(out, "\n");
    }

    fmt::print(out, "    constexpr {}_t() = default;\n", message.name);
    fmt::print(out, "\n");

    if (multiplexed)
    {
        fmt::print(out, "    constexpr sig_{}_t::Type& mux()\n", muxSignal->name);
        fmt::print(out, "    {{\n");
        fmt::print(out, "        return {};\n", muxSignal->name);
        fmt::print(out, "    }}\n");
        fmt::print(out, "\n");
    }

    generateBitHelpers(out);

    // encode
    fmt::print(out, "    constexpr std::array<uint8_t, {}u> encode() const\n", message.dlc);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        std::array<uint8_t, {}u> data{{}};\n", message.dlc);
    fmt::print(out, "        const std::span<uint8_t> out{{data}};\n");
    fmt::print(out, "\n");

    if (multiplexed)
    {
        fmt::print(out, "        insert_bits<sig_{}_t>(out, to_raw_u<sig_{}_t>({}));\n",
                   muxSignal->name, muxSignal->name, muxSignal->name);
        fmt::print(out, "\n");

        for (const auto &group : muxGroups)
        {
            fmt::print(out, "        if (static_cast<uint64_t>({}) == {}u)\n", muxSignal->name, group);
            fmt::print(out, "        {{\n");
            for (const auto &signal : message.signals)
            {
                if (!signal.isMultiplex || signal.isMultiplexor ||
                    (signal.multiplexedGroupIdx != group))
                {
                    continue;
                }
                fmt::print(out, "            insert_bits<sig_{}_t>(out, to_raw_u<sig_{}_t>({}));\n",
                           signal.name, signal.name, signal.name);
            }
            fmt::print(out, "        }}\n");
        }
        fmt::print(out, "\n");
    }

    for (const auto &signal : message.signals)
    {
        if (signal.isMultiplex || signal.isMultiplexor)
        {
            continue;
        }
        fmt::print(out, "        insert_bits<sig_{}_t>(out, to_raw_u<sig_{}_t>({}));\n",
                   signal.name, signal.name, signal.name);
    }

    fmt::print(out, "\n");
    fmt::print(out, "        return data;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // decode
    fmt::print(out, "    // False if the frame is shorter than this message, in which case\n");
    fmt::print(out, "    // nothing is written. A short frame used to be zero padded by the\n");
    fmt::print(out, "    // caller and decoded as though those zeroes were real readings.\n");
    fmt::print(out, "    constexpr bool decode(std::span<const uint8_t> data)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        if (data.size() < dlc)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            return false;\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "\n");

    if (multiplexed)
    {
        fmt::print(out, "        {} = extract<sig_{}_t>(data);\n", muxSignal->name, muxSignal->name);
        fmt::print(out, "\n");

        for (const auto &group : muxGroups)
        {
            fmt::print(out, "        if (static_cast<uint64_t>({}) == {}u)\n", muxSignal->name, group);
            fmt::print(out, "        {{\n");

            if (group == startMuxGroup)
            {
                fmt::print(out, "            // The lowest group index starts a batch, so the rest\n");
                fmt::print(out, "            // are cleared here to align the completeness check.\n");
                fmt::print(out, "            seen_mux_{} = true;\n", group);
                for (const auto &other : muxGroups)
                {
                    if (other != startMuxGroup)
                    {
                        fmt::print(out, "            seen_mux_{} = false;\n", other);
                    }
                }
            }
            else
            {
                fmt::print(out, "            seen_mux_{} = true;\n", group);
            }

            for (const auto &signal : message.signals)
            {
                if (!signal.isMultiplex || signal.isMultiplexor ||
                    (signal.multiplexedGroupIdx != group))
                {
                    continue;
                }
                fmt::print(out, "            {} = extract<sig_{}_t>(data);\n", signal.name,
                           signal.name);
            }

            fmt::print(out, "        }}\n");
        }
        fmt::print(out, "\n");
    }

    for (const auto &signal : message.signals)
    {
        if (signal.isMultiplex || signal.isMultiplexor)
        {
            continue;
        }
        fmt::print(out, "        {} = extract<sig_{}_t>(data);\n", signal.name, signal.name);
    }

    fmt::print(out, "\n");
    fmt::print(out, "        return true;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // visit
    for (std::string_view qualifier : {"", " const"})
    {
        fmt::print(out, "    template <typename Func>\n");
        fmt::print(out, "    constexpr void visit(Func&& fn){}\n", qualifier);
        fmt::print(out, "    {{\n");
        for (const auto &signal : message.signals)
        {
            fmt::print(out, "        fn({}, sig_{}_t{{}});\n", signal.name, signal.name);
        }
        if (message.signals.empty())
        {
            fmt::print(out, "        (void)fn;\n");
        }
        fmt::print(out, "    }}\n");
        fmt::print(out, "\n");
    }

    if (multiplexed)
    {
        fmt::print(out, "    constexpr bool all_multiplexed_indexes_seen() const\n");
        fmt::print(out, "    {{\n");
        fmt::print(out, "        return ");
        size_t i = 0;
        for (const auto &group : muxGroups)
        {
            fmt::print(out, "{}seen_mux_{}", (i == 0) ? "" : " && ", group);
            i += 1;
        }
        fmt::print(out, ";\n");
        fmt::print(out, "    }}\n");
        fmt::print(out, "\n");
        fmt::print(out, "    constexpr void clear_seen_multiplexed_indexes()\n");
        fmt::print(out, "    {{\n");
        for (const auto &group : muxGroups)
        {
            fmt::print(out, "        seen_mux_{} = false;\n", group);
        }
        fmt::print(out, "    }}\n");
        fmt::print(out, "\n");
    }

    fmt::print(out, "}};  // struct {}_t\n", message.name);
    fmt::print(out, "\n");
    fmt::print(out, "}}  // namespace {}\n", base);
    fmt::print(out, "\n");
    fmt::print(out, "#endif  // {}\n", guard);
}

} // namespace

void generate_cpp_common_header(const std::string &base, std::ostream &out)
{
    std::string baseUpper = base;
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(), ::toupper);

    // Anything shared by every message header lives here once. Emitting it per
    // message put several dozen definitions of the same enum in one namespace.
    fmt::print(out, "#ifndef {}_COMMON_H_\n", baseUpper);
    fmt::print(out, "#define {}_COMMON_H_\n\n", baseUpper);
    fmt::print(out, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(out, "\n");
    fmt::print(out, "#include <cstdint>\n");
    fmt::print(out, "\n");
    fmt::print(out, "namespace {}\n", base);
    fmt::print(out, "{{\n");
    fmt::print(out, "\n");
    fmt::print(out, "// How the raw bits of a signal are to be read, from SIG_VALTYPE_.\n");
    fmt::print(out, "enum class raw_encoding\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "    Integer,\n");
    fmt::print(out, "    Float,\n");
    fmt::print(out, "    Double,\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");
    fmt::print(out, "// Two to the power n, exactly, at compile time. Used for field bounds:\n");
    fmt::print(out, "// a power of two is always exactly representable as a double, which the\n");
    fmt::print(out, "// largest value of a wide field is not.\n");
    fmt::print(out, "constexpr double two_pow(uint32_t n)\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "    double result = 1.0;\n");
    fmt::print(out, "    for (uint32_t i = 0; i < n; ++i)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        result *= 2.0;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "    return result;\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");
    fmt::print(out, "// Round half away from zero, straight into the integer type the caller\n");
    fmt::print(out, "// wants. This is what DBC encoding needs; it is deliberately not a\n");
    fmt::print(out, "// general floor/ceil, because std::floor is only constexpr from C++26\n");
    fmt::print(out, "// and reimplementing all of its edge cases would buy nothing here.\n");
    fmt::print(out, "//\n");
    fmt::print(out, "// The caller must already have excluded values outside Int's range --\n");
    fmt::print(out, "// to_raw_u() saturates first -- because that conversion is otherwise\n");
    fmt::print(out, "// undefined rather than clamping.\n");
    fmt::print(out, "template <typename Int>\n");
    fmt::print(out, "constexpr Int round_half_away_from_zero(double value)\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "    return static_cast<Int>((value >= 0.0) ? (value + 0.5) : (value - 0.5));\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");
    fmt::print(out, "}}  // namespace {}\n", base);
    fmt::print(out, "\n");
    fmt::print(out, "#endif  // {}_COMMON_H_\n", baseUpper);
}

void generate_cpp_message_header(const dbc_parser::Message &message, const std::string &base,
                                 std::ostream &out)
{
    generateMessageHeader(message, base, out);
}

void generate_cpp_header(const dbc_parser::Database &db, const std::string &base,
                         std::ostream &hout)
{
    std::string baseUpper = base;
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(), ::toupper);

    fmt::print(hout, "#ifndef {}_H_\n", baseUpper);
    fmt::print(hout, "#define {}_H_\n", baseUpper);
    fmt::print(hout, "\n");
    fmt::print(hout, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(hout, "#include <array>\n");
    fmt::print(hout, "#include <cstdint>\n");
    fmt::print(hout, "#include <span>\n");
    fmt::print(hout, "#include <string_view>\n");
    fmt::print(hout, "\n");

    for (const auto &message : db.messages)
    {
        fmt::print(hout, "#include \"{}_{}.h\"\n", base, message.name);
    }
    fmt::print(hout, "\n");

    fmt::print(hout, "namespace {}\n", base);
    fmt::print(hout, "{{\n");

    std::set<uint32_t> messageIds;
    for (const auto &message : db.messages)
    {
        messageIds.insert(message.id);
    }

    fmt::print(hout, "struct {}_t\n", base);
    fmt::print(hout, "{{\n");
    fmt::print(hout, "    static constexpr std::string_view name = {};\n", stringLiteral(base));
    fmt::print(hout, "    static constexpr std::array<uint32_t, {}u> message_ids = {{{:#x}}};\n",
               messageIds.size(), fmt::join(messageIds, ", "));
    fmt::print(hout, "\n");

    fmt::print(hout, "    enum class Messages : uint32_t\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        Unknown = 0,\n");
    for (const auto &message : db.messages)
    {
        fmt::print(hout, "        {} = {:#x},\n", message.name, message.id);
    }
    fmt::print(hout, "    }};\n");
    fmt::print(hout, "\n");

    for (const auto &message : db.messages)
    {
        fmt::print(hout, "    {}_t {}{{}};\n", message.name, message.name);
    }
    fmt::print(hout, "\n");

    fmt::print(hout, "    constexpr {}_t() = default;\n", base);
    fmt::print(hout, "\n");
    fmt::print(hout, "    // Decodes into the matching member and names it. Unknown if the id\n");
    fmt::print(hout, "    // is not ours, and Unknown too if the frame was too short for the\n");
    fmt::print(hout, "    // message it claims to be -- in which case nothing was written.\n");
    fmt::print(hout, "    constexpr Messages decode(uint32_t message_id, std::span<const uint8_t> data)\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        switch (message_id)\n");
    fmt::print(hout, "        {{\n");
    for (const auto &message : db.messages)
    {
        fmt::print(hout, "        case {}_t::id:\n", message.name);
        fmt::print(hout, "            return {}.decode(data) ? Messages::{} : Messages::Unknown;\n",
                   message.name, message.name);
        fmt::print(hout, "\n");
    }
    fmt::print(hout, "        }}\n");
    fmt::print(hout, "\n");
    fmt::print(hout, "        return Messages::Unknown;\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "\n");

    fmt::print(hout, "    // Hands the member struct for `msg` to `fn`. False if there is no\n");
    fmt::print(hout, "    // such message, in which case fn is not called.\n");
    for (std::string_view qualifier : {"", " const"})
    {
        fmt::print(hout, "    template <typename Func>\n");
        fmt::print(hout, "    constexpr bool visit_message(Messages msg, Func&& fn){}\n", qualifier);
        fmt::print(hout, "    {{\n");
        fmt::print(hout, "        switch (msg)\n");
        fmt::print(hout, "        {{\n");
        for (const auto &message : db.messages)
        {
            fmt::print(hout, "        case Messages::{}:\n", message.name);
            fmt::print(hout, "            fn({});\n", message.name);
            fmt::print(hout, "            return true;\n");
            fmt::print(hout, "\n");
        }
        fmt::print(hout, "        case Messages::Unknown:\n");
        fmt::print(hout, "            break;\n");
        fmt::print(hout, "        }}\n");
        fmt::print(hout, "\n");
        fmt::print(hout, "        return false;\n");
        fmt::print(hout, "    }}\n");
        fmt::print(hout, "\n");
    }

    fmt::print(hout, "    static constexpr std::string_view get_message_name(Messages msg) noexcept\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        switch (msg)\n");
    fmt::print(hout, "        {{\n");
    for (const auto &message : db.messages)
    {
        fmt::print(hout, "        case Messages::{}:\n", message.name);
        fmt::print(hout, "            return {};\n", stringLiteral(message.name));
        fmt::print(hout, "\n");
    }
    fmt::print(hout, "        case Messages::Unknown:\n");
    fmt::print(hout, "            break;\n");
    fmt::print(hout, "        }}\n");
    fmt::print(hout, "\n");
    fmt::print(hout, "        return \"Unknown\";\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "\n");

    fmt::print(hout, "    static constexpr std::string_view get_message_name(uint32_t message_id) noexcept\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        switch (message_id)\n");
    fmt::print(hout, "        {{\n");
    for (const auto &message : db.messages)
    {
        fmt::print(hout, "        case {}_t::id:\n", message.name);
        fmt::print(hout, "            return {};\n", stringLiteral(message.name));
        fmt::print(hout, "\n");
    }
    fmt::print(hout, "        }}\n");
    fmt::print(hout, "\n");
    fmt::print(hout, "        return \"Unknown\";\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "}};\n");
    fmt::print(hout, "}}  // namespace {}\n", base);
    fmt::print(hout, "#endif  // {}_H_\n", baseUpper);
}

} // namespace dbc_codegen
