#include "dbc_parser/generate_h.h"

#include "dbc_parser/dbc_parser.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
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
std::string doubleLiteral(double value)
{
    std::string text = fmt::format("{:.17g}", value);
    if (text.find_first_of(".eE") == std::string::npos)
    {
        text += ".0";
    }
    return text;
}

// The float nearest the DBC's value, spelled so it can only be a float.
std::string floatLiteral(double value)
{
    std::string text = fmt::format("{:.9g}", static_cast<float>(value));
    if (text.find_first_of(".eE") == std::string::npos)
    {
        text += ".0";
    }
    return text + "f";
}

// Ranges are worked out in 128 bits: the product of a 64 bit raw value and a
// 63 bit scale is exact there, and in no narrower type.
using i128 = __int128;

const i128 kInt64Min = std::numeric_limits<int64_t>::min();
const i128 kInt64Max = std::numeric_limits<int64_t>::max();

std::string decimal(i128 value)
{
    if (value == 0)
    {
        return "0";
    }

    const bool negative = (value < 0);
    unsigned __int128 magnitude = negative ? (static_cast<unsigned __int128>(0) - static_cast<unsigned __int128>(value))
                                           : static_cast<unsigned __int128>(value);
    std::string digits;
    while (magnitude != 0)
    {
        digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
        magnitude /= 10;
    }
    if (negative)
    {
        digits.push_back('-');
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

// An integer literal that means the same value whatever type it initialises.
// INT64_MIN cannot be written directly: the literal is its positive half,
// which overflows before the minus sign applies.
std::string integerLiteral(i128 value)
{
    if (value == kInt64Min)
    {
        return "(-9223372036854775807LL - 1)";
    }
    if (value > kInt64Max)
    {
        return decimal(value) + "ULL";
    }
    if ((value > std::numeric_limits<int32_t>::max()) || (value < std::numeric_limits<int32_t>::min()))
    {
        return decimal(value) + "LL";
    }
    return decimal(value);
}

struct IntChoice
{
    std::string_view name;
    i128 lo;
    i128 hi;
};

template <typename T>
constexpr IntChoice choice(std::string_view name)
{
    return {name, std::numeric_limits<T>::min(), std::numeric_limits<T>::max()};
}

const std::array<IntChoice, 4> kUnsignedTypes{{
    choice<uint8_t>("uint8_t"),
    choice<uint16_t>("uint16_t"),
    choice<uint32_t>("uint32_t"),
    choice<uint64_t>("uint64_t"),
}};

const std::array<IntChoice, 4> kSignedTypes{{
    choice<int8_t>("int8_t"),
    choice<int16_t>("int16_t"),
    choice<int32_t>("int32_t"),
    choice<int64_t>("int64_t"),
}};

// The narrowest standard integer type holding [lo, hi]: unsigned when nothing
// is negative, so a value that can never be negative is not typed as if it
// could be.
std::optional<IntChoice> smallestHolding(i128 lo, i128 hi)
{
    const auto &table = (lo >= 0) ? kUnsignedTypes : kSignedTypes;
    for (const auto &choice : table)
    {
        if ((lo >= choice.lo) && (hi <= choice.hi))
        {
            return choice;
        }
    }
    return std::nullopt;
}

// Whether a named integer type can hold a value. The declared [min|max] is
// checked against the signal's own type before being emitted in it.
bool fitsType(std::string_view name, i128 value)
{
    for (const auto &table : {kUnsignedTypes, kSignedTypes})
    {
        for (const auto &entry : table)
        {
            if (entry.name == name)
            {
                return (value >= entry.lo) && (value <= entry.hi);
            }
        }
    }
    return false;
}

// A DBC number as an exact integer, if it is one small enough to multiply a
// 64 bit raw value by without leaving 128 bits.
bool asInteger(double value, i128 &out)
{
    if (!std::isfinite(value) || (std::floor(value) != value) || (std::fabs(value) >= 0x1p63))
    {
        return false;
    }
    out = static_cast<i128>(static_cast<long long>(value));
    return true;
}

std::string_view rawTypeName(uint32_t length)
{
    if (length <= 8u)
    {
        return "uint8_t";
    }
    if (length <= 16u)
    {
        return "uint16_t";
    }
    if (length <= 32u)
    {
        return "uint32_t";
    }
    return "uint64_t";
}

// The integer a floating-point signal is rounded into on encode: wide enough
// for every raw value of the field, and no wider.
std::string_view conversionTypeName(uint32_t length, bool isSigned)
{
    if ((length < 32u) || ((length == 32u) && isSigned))
    {
        return "int32_t";
    }
    if (length == 32u)
    {
        return "uint32_t";
    }
    if ((length < 64u) || isSigned)
    {
        return "int64_t";
    }
    return "uint64_t";
}

// How a signal's raw bits become its value. Mirrors value_domain in the
// generated common header; see generate_cpp_common_header().
enum class Domain
{
    Bool,
    Enum,
    Integer,
    Float,
    Double,
    IeeeFloat,
    IeeeDouble,
};

std::string_view domainName(Domain domain)
{
    switch (domain)
    {
    case Domain::Bool:
        return "Bool";

    case Domain::Enum:
        return "Enum";

    case Domain::Integer:
        return "Integer";

    case Domain::Float:
        return "Float";

    case Domain::Double:
        return "Double";

    case Domain::IeeeFloat:
        return "IeeeFloat";

    case Domain::IeeeDouble:
        return "IeeeDouble";
    }

    return "Double";
}

// Everything the generated code needs to know about one signal's types,
// decided once so that emission only ever reads it.
struct SignalPlan
{
    Domain domain{Domain::Double};
    bool identity{false};

    std::string rawType;
    std::string type;
    std::string enumBase;     // Enum only
    std::string workType;     // Integer with scaling only
    std::string convType;     // Float and Double only
    std::string constantType; // the type of scale and offset
    std::string scale;
    std::string offset;

    i128 rawMin{};
    i128 rawMax{};
    i128 physMin{};
    i128 physMax{};
};

// A float32 decode and encode keep every raw step recoverable while
// max|raw| + |offset / scale| stays below this. The provable bound for a
// float round trip is 2^20.4; the first failure an adversarial search found
// was 2^22.75. See docs/libs/dbc_parser.md.
constexpr double kFloatStepsFromZero = 0x1p20;

bool floatIsNormal(double value)
{
    if (!std::isfinite(value) || (std::fabs(value) > FLT_MAX))
    {
        return false;
    }
    return std::fpclassify(static_cast<float>(value)) == FP_NORMAL;
}

void setIntegerConstants(SignalPlan &plan, std::string_view constantType, i128 scale, i128 offset)
{
    plan.constantType = constantType;
    plan.scale = integerLiteral(scale);
    plan.offset = integerLiteral(offset);
}

// The type rules, in order. Ranges always come from the raw bit range, never
// from the DBC's declared minimum and maximum, which are [0|0] in about half of
// the files this runs on. Those are emitted separately, in the signal's own
// type, and rail the value only when the file actually set them.
SignalPlan planSignal(const dbc_parser::Signal &signal)
{
    SignalPlan plan;
    plan.identity = (signal.scale == 1.0) && (signal.offset == 0.0);
    plan.rawType = rawTypeName(signal.length);

    const uint32_t length = std::clamp(signal.length, 1u, 64u);
    plan.rawMin = signal.isSigned ? -(static_cast<i128>(1) << (length - 1u)) : 0;
    plan.rawMax = signal.isSigned ? ((static_cast<i128>(1) << (length - 1u)) - 1)
                                  : ((static_cast<i128>(1) << length) - 1);

    switch (signal.valueType)
    {
    case dbc_parser::SignalValueType::Float:
        // No arithmetic at all when unscaled, so the bits survive untouched --
        // including -0.0, which 0.0f * 1.0f + 0.0f would not.
        plan.domain = Domain::IeeeFloat;
        plan.type = plan.identity ? "float" : "double";
        plan.constantType = plan.type;
        plan.scale = plan.identity ? floatLiteral(signal.scale) : doubleLiteral(signal.scale);
        plan.offset = plan.identity ? floatLiteral(signal.offset) : doubleLiteral(signal.offset);
        return plan;

    case dbc_parser::SignalValueType::Double:
        plan.domain = Domain::IeeeDouble;
        plan.type = "double";
        plan.constantType = "double";
        plan.scale = doubleLiteral(signal.scale);
        plan.offset = doubleLiteral(signal.offset);
        return plan;

    case dbc_parser::SignalValueType::Integer:
        break;
    }

    // A value table becomes an enum only when raw and physical are the same
    // number. VAL_ maps *raw* values to names, so with a scale or offset in
    // play the enumerators would name unscaled values while the field held a
    // scaled one -- two different numbers wearing the same name.
    if (!signal.valueTable.empty() && plan.identity)
    {
        i128 lo = plan.rawMin;
        i128 hi = plan.rawMax;
        for (const auto &mapping : signal.valueTable)
        {
            lo = std::min(lo, static_cast<i128>(mapping.rawValue));
            hi = std::max(hi, static_cast<i128>(mapping.rawValue));
        }
        const auto base = smallestHolding(lo, hi);
        plan.domain = Domain::Enum;
        plan.type = "Values";
        plan.enumBase = base ? std::string(base->name) : "int64_t";
        setIntegerConstants(plan, "int32_t", 1, 0);
        return plan;
    }

    if ((length == 1u) && !signal.isSigned && plan.identity)
    {
        plan.domain = Domain::Bool;
        plan.type = "bool";
        setIntegerConstants(plan, "int32_t", 1, 0);
        return plan;
    }

    i128 scale = 0;
    i128 offset = 0;
    if (asInteger(signal.scale, scale) && asInteger(signal.offset, offset))
    {
        const i128 atMin = plan.rawMin * scale + offset;
        const i128 atMax = plan.rawMax * scale + offset;
        const i128 lo = std::min(atMin, atMax);
        const i128 hi = std::max(atMin, atMax);

        if (const auto type = smallestHolding(lo, hi))
        {
            if (plan.identity)
            {
                plan.domain = Domain::Integer;
                plan.type = type->name;
                plan.physMin = lo;
                plan.physMax = hi;
                setIntegerConstants(plan, "int32_t", 1, 0);
                return plan;
            }

            // Encode clamps the value to [lo, hi] before subtracting the
            // offset, so the arithmetic only ever meets these numbers. Holding
            // the raw range also rules out INT_MIN / -1, and holding |scale|
            // rules out negating the scale.
            const std::array<i128, 7> needed{plan.rawMin,         plan.rawMax,         plan.rawMin * scale,
                                             plan.rawMax * scale, lo,                  hi,
                                             (scale < 0) ? -scale : scale};
            for (const auto &work : {kSignedTypes[2], kSignedTypes[3]})
            {
                const bool fits = std::all_of(needed.begin(), needed.end(), [&](i128 n) {
                    return (n >= work.lo) && (n <= work.hi);
                });
                if (fits)
                {
                    plan.domain = Domain::Integer;
                    plan.type = type->name;
                    plan.workType = work.name;
                    plan.physMin = lo;
                    plan.physMax = hi;
                    setIntegerConstants(plan, work.name, scale, offset);
                    return plan;
                }
            }
        }
        // Integral, but too wide for 64 bit arithmetic: falls through to the
        // floating-point rule like any other scaling.
    }

    const double maxAbsRaw = signal.isSigned ? std::ldexp(1.0, static_cast<int>(length) - 1)
                                             : (std::ldexp(1.0, static_cast<int>(length)) - 1.0);
    const double stepsFromZero = maxAbsRaw + std::fabs(signal.offset / signal.scale);

    // The threshold assumes the constants are ordinary floats. A scale that
    // underflows to zero or a subnormal would decode everything as nothing.
    const bool floatable = (stepsFromZero < kFloatStepsFromZero) && floatIsNormal(signal.scale) &&
                           ((signal.offset == 0.0) || floatIsNormal(signal.offset));

    if (floatable)
    {
        plan.domain = Domain::Float;
        plan.type = "float";
        plan.convType = "int32_t";
        plan.constantType = "float";
        plan.scale = floatLiteral(signal.scale);
        plan.offset = floatLiteral(signal.offset);
        return plan;
    }

    plan.domain = Domain::Double;
    plan.type = "double";
    plan.convType = conversionTypeName(length, signal.isSigned);
    plan.constantType = "double";
    plan.scale = doubleLiteral(signal.scale);
    plan.offset = doubleLiteral(signal.offset);
    return plan;
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


// The file's declared [min|max], in the signal's own type.
//
// It used to be double for every signal. A limit wider than the value it
// bounds forces every comparison through floating point, and on a float signal
// it puts the boundary between two representable values.
//
// has_range is what gates the rail in the generated code. It is false when the
// file declares [0|0], which is about half the signals in the wild, so an
// unset declaration stays inert and a real one becomes enforcement.
//
// Bool and Enum keep double limits and are never railed: a bool cannot leave
// its range, and a declared numeric bound on an enum class need not be an
// enumerator.
//
// A declared limit an integer signal's type cannot hold is NOT an error. This
// generator has no diagnostic channel, and failing a consumer's build over a
// vendor DBC nobody here controls is the wrong trade. Such a signal keeps
// double limits, is not railed, and says so in the generated header.
struct DeclaredLimits
{
    std::string type;
    std::string min;
    std::string max;
    bool hasRange{false};
    std::string note;
};

DeclaredLimits declaredLimits(const dbc_parser::Signal &signal, const SignalPlan &plan)
{
    DeclaredLimits limits;

    if ((plan.domain == Domain::Bool) || (plan.domain == Domain::Enum))
    {
        limits.type = "double";
        limits.min = doubleLiteral(signal.minimum);
        limits.max = doubleLiteral(signal.maximum);
        return limits;
    }

    if (plan.domain == Domain::Integer)
    {
        i128 lo = 0;
        i128 hi = 0;
        if (asInteger(signal.minimum, lo) && asInteger(signal.maximum, hi) && fitsType(plan.type, lo) &&
            fitsType(plan.type, hi))
        {
            limits.type = plan.type;
            limits.min = integerLiteral(lo);
            limits.max = integerLiteral(hi);
            limits.hasRange = (lo != hi);
            return limits;
        }

        limits.type = "double";
        limits.min = doubleLiteral(signal.minimum);
        limits.max = doubleLiteral(signal.maximum);
        limits.note = "the declared range does not fit this signal's type, so it is not railed";
        return limits;
    }

    // Compared AFTER narrowing: two declared doubles can land on one float, and
    // railing to a single point is not what the file meant.
    const bool asFloat = (plan.type == "float");
    limits.type = plan.type;
    limits.min = asFloat ? floatLiteral(signal.minimum) : doubleLiteral(signal.minimum);
    limits.max = asFloat ? floatLiteral(signal.maximum) : doubleLiteral(signal.maximum);

    // A SIG_VALTYPE_ signal is NEVER railed, whatever the file declares.
    //
    // Its [min|max] describes the field as though the bits were an integer:
    // dbc_test_features declares AsFloat as [-2147483648|2147483647], the int32
    // span of its 32 bits, and dbc_test_precision declares IeeeScaled as
    // rawMin*0.5+10 .. rawMax*0.5+10. The bits are an IEEE float, so the value
    // runs to 3.4e38 and the declaration is not in the same units as the value.
    // Railing to it destroys every large reading. MEASURED: doing so put 302
    // disagreements into the cantools golden corpus, on all three IEEE signals
    // and on nothing else.
    if ((plan.domain == Domain::IeeeFloat) || (plan.domain == Domain::IeeeDouble))
    {
        return limits;
    }

    limits.hasRange = asFloat ? (static_cast<float>(signal.minimum) != static_cast<float>(signal.maximum))
                              : (signal.minimum != signal.maximum);
    return limits;
}

void generateSignalTraits(const dbc_parser::Signal &signal, const SignalPlan &plan, std::ostream &out)
{
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

    fmt::print(out, "        static constexpr value_domain domain = value_domain::{};\n", domainName(plan.domain));
    fmt::print(out, "        static constexpr bool identity = {};\n", plan.identity);
    fmt::print(out, "\n");

    if (plan.domain == Domain::Enum)
    {
        const std::vector<std::string> names = enumeratorNames(signal);

        fmt::print(out, "        enum class Values : {}\n", plan.enumBase);
        fmt::print(out, "        {{\n");
        for (size_t i = 0; i < names.size(); ++i)
        {
            fmt::print(out, "            {} = {},\n", names[i],
                       integerLiteral(static_cast<i128>(signal.valueTable[i].rawValue)));
        }
        fmt::print(out, "        }};\n");
        fmt::print(out, "\n");
    }

    fmt::print(out, "        using Raw = {};\n", plan.rawType);
    fmt::print(out, "        // The type decoded values of this signal are handed over as.\n");
    fmt::print(out, "        using Type = {};\n", plan.type);
    if (!plan.workType.empty())
    {
        fmt::print(out, "        using Work = {};\n", plan.workType);
    }
    if (!plan.convType.empty())
    {
        fmt::print(out, "        using Conv = {};\n", plan.convType);
    }
    fmt::print(out, "\n");

    // Scale and offset are emitted for every signal, including enumerated
    // ones. Leaving them off value-table signals meant a scaled enum lost its
    // scaling entirely, and callers had no way to notice.
    fmt::print(out, "        static constexpr {} scale = {};\n", plan.constantType, plan.scale);
    fmt::print(out, "        static constexpr {} offset = {};\n", plan.constantType, plan.offset);

    if (plan.domain == Domain::Integer)
    {
        fmt::print(out, "        static constexpr {} phys_min = {};\n", plan.type, integerLiteral(plan.physMin));
        fmt::print(out, "        static constexpr {} phys_max = {};\n", plan.type, integerLiteral(plan.physMax));
    }
    if (!plan.convType.empty())
    {
        fmt::print(out, "        static constexpr {} raw_min = {};\n", plan.convType, integerLiteral(plan.rawMin));
        fmt::print(out, "        static constexpr {} raw_max = {};\n", plan.convType, integerLiteral(plan.rawMax));
    }
    fmt::print(out, "\n");

    const DeclaredLimits limits = declaredLimits(signal, plan);
    if (!limits.note.empty())
    {
        fmt::print(out, "        // NOTE: {}.\n", limits.note);
    }
    fmt::print(out, "        // As declared in the DBC, in this signal's own type. has_range is\n");
    fmt::print(out, "        // false when the file declares [0|0], which is about half of them.\n");
    fmt::print(out, "        static constexpr {} minimum = {};\n", limits.type, limits.min);
    fmt::print(out, "        static constexpr {} maximum = {};\n", limits.type, limits.max);
    fmt::print(out, "        static constexpr bool has_range = {};\n", limits.hasRange);
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

// A name for a local in a generated member function that no signal member
// shares. A local that shadows a member is a -Wshadow error, and a signal
// called `frame` is not something the DBC author should have to avoid.
std::string localName(const dbc_parser::Message &message, std::string base)
{
    const auto taken = [&](const std::string &name) {
        return std::any_of(message.signals.begin(), message.signals.end(),
                           [&](const dbc_parser::Signal &signal) { return signal.name == name; });
    };
    while (taken(base))
    {
        base.push_back('_');
    }
    return base;
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

    const std::string frame = localName(message, "frame");
    const std::string view = localName(message, "view");
    const std::string muxRaw = localName(message, "mux_raw");
    const std::string fn = localName(message, "fn");

    std::string guard = base + "_" + message.name + "_H_";
    std::transform(guard.begin(), guard.end(), guard.begin(), ::toupper);

    fmt::print(out, "#ifndef {}\n", guard);
    fmt::print(out, "#define {}\n\n", guard);
    fmt::print(out, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(out, "#include <array>\n");
    fmt::print(out, "#include <cstdint>\n");
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
        generateSignalTraits(signal, planSignal(signal), out);
    }

    if (multiplexed)
    {
        // Group indexes are raw multiplexor bit patterns, which is what the
        // gating compares, so they are typed as the multiplexor's Raw. They
        // come after the traits because the array's element type needs them.
        fmt::print(out, "    static constexpr std::array<sig_{}_t::Raw, {}u> multiplexor_group_indexes = {{{}}};\n",
                   muxSignal->name, muxGroups.size(), fmt::join(muxGroups, ", "));
        fmt::print(out, "    static constexpr sig_{}_t::Raw start_mux_group_index = {}u;\n", muxSignal->name,
                   startMuxGroup);
        fmt::print(out, "\n");
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

    // encode
    fmt::print(out, "    [[nodiscard]] constexpr std::array<uint8_t, {}u> encode() const\n", message.dlc);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        std::array<uint8_t, {}u> {}{{}};\n", message.dlc, frame);
    fmt::print(out, "        const std::span<uint8_t> {}{{{}}};\n", view, frame);
    fmt::print(out, "\n");

    if (multiplexed)
    {
        // Groups are selected by the multiplexor's raw bits, held in the
        // accumulator word rather than the signal's own type: a group index
        // compared against a uint8_t would be a comparison clang can prove
        // false, and -Werror makes that a build failure.
        fmt::print(out, "        const dbc_detail::acc_t<sig_{0}_t> {1} = dbc_detail::to_raw<sig_{0}_t>({0});\n",
                   muxSignal->name, muxRaw);
        fmt::print(out, "        dbc_detail::insert_bits<sig_{}_t>({}, {});\n", muxSignal->name, view, muxRaw);
        fmt::print(out, "\n");

        for (const auto &group : muxGroups)
        {
            fmt::print(out, "        if ({} == {}u)\n", muxRaw, group);
            fmt::print(out, "        {{\n");
            for (const auto &signal : message.signals)
            {
                if (!signal.isMultiplex || signal.isMultiplexor ||
                    (signal.multiplexedGroupIdx != group))
                {
                    continue;
                }
                fmt::print(out, "            dbc_detail::encode_signal<sig_{0}_t>({1}, {0});\n",
                           signal.name, view);
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
        fmt::print(out, "        dbc_detail::encode_signal<sig_{0}_t>({1}, {0});\n", signal.name, view);
    }

    fmt::print(out, "\n");
    fmt::print(out, "        return {};\n", frame);
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // decode
    fmt::print(out, "    // False if the frame is shorter than this message, in which case\n");
    fmt::print(out, "    // nothing is written. A short frame used to be zero padded by the\n");
    fmt::print(out, "    // caller and decoded as though those zeroes were real readings.\n");
    fmt::print(out, "    [[nodiscard]] constexpr bool decode(std::span<const uint8_t> {})\n", frame);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        if ({}.size() < dlc)\n", frame);
    fmt::print(out, "        {{\n");
    fmt::print(out, "            return false;\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "\n");

    if (multiplexed)
    {
        fmt::print(out, "        const dbc_detail::acc_t<sig_{0}_t> {1} = dbc_detail::extract_bits<sig_{0}_t>({2});\n",
                   muxSignal->name, muxRaw, frame);
        fmt::print(out, "        {0} = dbc_detail::from_raw<sig_{0}_t>(static_cast<sig_{0}_t::Raw>({1}));\n",
                   muxSignal->name, muxRaw);
        fmt::print(out, "\n");

        for (const auto &group : muxGroups)
        {
            fmt::print(out, "        if ({} == {}u)\n", muxRaw, group);
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
                fmt::print(out, "            {0} = dbc_detail::decode_signal<sig_{0}_t>({1});\n",
                           signal.name, frame);
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
        fmt::print(out, "        {0} = dbc_detail::decode_signal<sig_{0}_t>({1});\n", signal.name, frame);
    }

    fmt::print(out, "\n");
    fmt::print(out, "        return true;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // visit
    for (std::string_view qualifier : {"", " const"})
    {
        fmt::print(out, "    template <typename Func>\n");
        fmt::print(out, "    constexpr void visit(Func&& {}){}\n", fn, qualifier);
        fmt::print(out, "    {{\n");
        for (const auto &signal : message.signals)
        {
            fmt::print(out, "        {}({}, sig_{}_t{{}});\n", fn, signal.name, signal.name);
        }
        if (message.signals.empty())
        {
            fmt::print(out, "        (void){};\n", fn);
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

// Shared by every message header of one database, emitted once. Written as one
// block of C++ rather than line by line so it reads as the code it produces.
constexpr std::string_view kCommonHeader = R"CPP(#ifndef @GUARD@
#define @GUARD@

/* Generated C++ header - do not edit as any changes will be overwritten. */

#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace @BASE@
{

// How a signal's raw bits become its value. The generator picks one per signal
// from the field's raw bit range -- never from the DBC's declared minimum and
// maximum, which are [0|0] in half the files in the wild. Those still rail the
// decoded and encoded value when the file set them; see rail():
//
//   Bool        one unsigned bit, no scaling, no value table
//   Enum        a value table on an unscaled field
//   Integer     integral scale and offset: integer arithmetic only, no FPU
//   Float       float32 arithmetic. Every raw value still round-trips, because
//               max|raw| + |offset / scale| < 2^20.
//   Double      double arithmetic, for everything else
//   IeeeFloat   SIG_VALTYPE_ float32 on the wire
//   IeeeDouble  SIG_VALTYPE_ float64 on the wire
enum class value_domain
{
    Bool,
    Enum,
    Integer,
    Float,
    Double,
    IeeeFloat,
    IeeeDouble,
};

namespace dbc_detail
{

// The word raw bits are gathered in. 32 bits whenever the field fits, so a
// 32 bit core never does 64 bit shifts for an 8 bit signal.
template <typename Sig>
using acc_t = std::conditional_t<(Sig::length <= 32u), uint32_t, uint64_t>;

template <typename Sig>
using sacc_t = std::make_signed_t<acc_t<Sig>>;

// Sig::length ones, right aligned. Shifting down from all ones keeps the
// shift count in range for every length, a full 64 bit field included.
template <typename Sig>
constexpr acc_t<Sig> field_mask()
{
    return static_cast<acc_t<Sig>>(~acc_t<Sig>{0} >> (std::numeric_limits<acc_t<Sig>>::digits - Sig::length));
}

// Sig::length bits out of the frame, right aligned. Motorola order walks down
// within a byte, then jumps to the top of the next one.
template <typename Sig>
constexpr acc_t<Sig> extract_bits(std::span<const uint8_t> frame)
{
    acc_t<Sig> raw = 0u;
    if constexpr (Sig::little_endian)
    {
        for (uint32_t i = 0u; i < Sig::length; ++i)
        {
            const uint32_t position = Sig::start_bit + i;
            const auto bit = static_cast<acc_t<Sig>>((frame[position / 8u] >> (position % 8u)) & 1u);
            raw = static_cast<acc_t<Sig>>(raw | (bit << i));
        }
    }
    else
    {
        uint32_t position = Sig::start_bit;
        for (uint32_t i = 0u; i < Sig::length; ++i)
        {
            const auto bit = static_cast<acc_t<Sig>>((frame[position / 8u] >> (position % 8u)) & 1u);
            raw = static_cast<acc_t<Sig>>((raw << 1u) | bit);
            position = ((position % 8u) == 0u) ? (position + 15u) : (position - 1u);
        }
    }
    return raw;
}

constexpr void set_bit(std::span<uint8_t> frame, uint32_t position, bool on)
{
    const auto mask = static_cast<uint8_t>(1u << (position % 8u));
    uint8_t &byte = frame[position / 8u];
    byte = on ? static_cast<uint8_t>(byte | mask) : static_cast<uint8_t>(byte & static_cast<uint8_t>(~mask));
}

template <typename Sig>
constexpr void insert_bits(std::span<uint8_t> frame, acc_t<Sig> raw)
{
    if constexpr (Sig::little_endian)
    {
        for (uint32_t i = 0u; i < Sig::length; ++i)
        {
            set_bit(frame, Sig::start_bit + i, ((raw >> i) & 1u) != 0u);
        }
    }
    else
    {
        uint32_t position = Sig::start_bit;
        for (uint32_t i = 0u; i < Sig::length; ++i)
        {
            set_bit(frame, position, ((raw >> (Sig::length - 1u - i)) & 1u) != 0u);
            position = ((position % 8u) == 0u) ? (position + 15u) : (position - 1u);
        }
    }
}

// Two's complement sign extension of a Sig::length bit field. (raw ^ sign) -
// sign in unsigned arithmetic is exact for every length, including a field
// that fills the whole word, where shifting ~0 left would not be.
template <typename Sig>
constexpr sacc_t<Sig> sign_extend(acc_t<Sig> raw)
{
    const auto sign = static_cast<acc_t<Sig>>(acc_t<Sig>{1} << (Sig::length - 1u));
    return static_cast<sacc_t<Sig>>(static_cast<acc_t<Sig>>((raw ^ sign) - sign));
}

// Round half away from zero into [lo, hi], saturating everything else.
//
// NaN and anything below the field go to lo, anything at or above it to hi.
// The fraction is taken after truncation rather than by adding 0.5 first,
// which rounds 0.49999997f up to 1.
template <typename Fp, typename Int>
constexpr Int round_saturate(Fp q, Int lo, Int hi)
{
    if (!(q >= static_cast<Fp>(lo)))
    {
        return lo;
    }
    if (q >= static_cast<Fp>(hi))
    {
        return hi;
    }
    const Int truncated = static_cast<Int>(q);
    const Fp fraction = q - static_cast<Fp>(truncated);
    if (fraction >= static_cast<Fp>(0.5))
    {
        return static_cast<Int>(truncated + 1);
    }
    if (fraction <= static_cast<Fp>(-0.5))
    {
        return static_cast<Int>(truncated - 1);
    }
    return truncated;
}

// The file's declared [min|max], applied to a value. PROVIDED BUT NOT APPLIED.
//
// Nothing calls this. decode and encode deliberately do not rail, because the
// declared range in real files is not trustworthy as a value constraint, and
// clamping to it silently rewrites good data:
//
//   msel_master_relay.dbc declares temperature_internal, a SIGNED 16-bit field,
//   as [0|125]. A cold car reads -10C. Railing to the declaration reports 0C --
//   a plausible number, silently wrong, on an ordinary signal. Its neighbour
//   load_current is declared [-255|600] and is fine, so the difference is only
//   whether the author happened to write a sensible bound.
//
//   A SIG_VALTYPE_ signal is worse: its [min|max] describes the raw field, not
//   the value, so railing destroyed every large reading and put 302
//   disagreements into the cantools golden corpus.
//
// Encode has the same exposure as decode: a caller that legitimately sends -10
// would have it clamped to 0 on the way to the wire. Call this explicitly if a
// particular consumer wants the policy; do not wire it back into from_raw or
// to_raw without first establishing that the files being read declare ranges
// that mean what they say.
//
// has_range is false when the declaration is [0|0].
//
// Bool, Enum and the SIG_VALTYPE_ domains are never railed, so has_range is
// false for them whatever the file said: a bool cannot leave its range, a
// declared numeric bound on an enum class need not be an enumerator, and an
// IEEE signal's declared range describes its raw field rather than its value.
template <typename Sig>
constexpr typename Sig::Type rail(typename Sig::Type value)
{
    if constexpr (Sig::has_range)
    {
        if (value < Sig::minimum)
        {
            return Sig::minimum;
        }
        if (value > Sig::maximum)
        {
            return Sig::maximum;
        }
    }
    return value;
}

// Raw bits to the value they mean.
template <typename Sig>
constexpr typename Sig::Type from_raw(typename Sig::Raw raw)
{
    const acc_t<Sig> bits = raw;

    if constexpr (Sig::domain == value_domain::Bool)
    {
        return bits != 0u;
    }
    else if constexpr (Sig::domain == value_domain::Enum)
    {
        if constexpr (Sig::is_signed)
        {
            return static_cast<typename Sig::Type>(sign_extend<Sig>(bits));
        }
        else
        {
            return static_cast<typename Sig::Type>(bits);
        }
    }
    else if constexpr (Sig::domain == value_domain::Integer)
    {
        if constexpr (Sig::identity)
        {
            if constexpr (Sig::is_signed)
            {
                return static_cast<typename Sig::Type>(sign_extend<Sig>(bits));
            }
            else
            {
                return static_cast<typename Sig::Type>(bits);
            }
        }
        else if constexpr (Sig::is_signed)
        {
            const auto n = static_cast<typename Sig::Work>(sign_extend<Sig>(bits));
            return static_cast<typename Sig::Type>(n * Sig::scale + Sig::offset);
        }
        else
        {
            const auto n = static_cast<typename Sig::Work>(bits);
            return static_cast<typename Sig::Type>(n * Sig::scale + Sig::offset);
        }
    }
    else if constexpr (Sig::domain == value_domain::Float)
    {
        static_assert(std::is_same_v<std::remove_cv_t<decltype(Sig::scale)>, float>,
                      "a float signal's constants must be float, or its arithmetic silently runs in double");
        if constexpr (Sig::is_signed)
        {
            return static_cast<float>(static_cast<int32_t>(sign_extend<Sig>(bits))) * Sig::scale + Sig::offset;
        }
        else
        {
            return static_cast<float>(static_cast<int32_t>(bits)) * Sig::scale + Sig::offset;
        }
    }
    else if constexpr (Sig::domain == value_domain::Double)
    {
        if constexpr (Sig::is_signed)
        {
            return static_cast<double>(sign_extend<Sig>(bits)) * Sig::scale + Sig::offset;
        }
        else
        {
            return static_cast<double>(bits) * Sig::scale + Sig::offset;
        }
    }
    else if constexpr (Sig::domain == value_domain::IeeeFloat)
    {
        const float value = std::bit_cast<float>(static_cast<uint32_t>(bits));
        if constexpr (Sig::identity)
        {
            return value;
        }
        else
        {
            return static_cast<double>(value) * Sig::scale + Sig::offset;
        }
    }
    else
    {
        const double value = std::bit_cast<double>(static_cast<uint64_t>(bits));
        if constexpr (Sig::identity)
        {
            return value;
        }
        else
        {
            return value * Sig::scale + Sig::offset;
        }
    }
}

// A value to the raw bits that encode it: saturated to the field, rounded
// half away from zero where the scale leaves a fraction.
template <typename Sig>
constexpr typename Sig::Raw to_raw(typename Sig::Type value)
{
    if constexpr (Sig::domain == value_domain::Bool)
    {
        return static_cast<typename Sig::Raw>(value ? 1u : 0u);
    }
    else if constexpr (Sig::domain == value_domain::Enum)
    {
        return static_cast<typename Sig::Raw>(static_cast<acc_t<Sig>>(std::to_underlying(value)) & field_mask<Sig>());
    }
    else if constexpr (Sig::domain == value_domain::Integer)
    {
        // Clamped first, so the arithmetic below never meets a value the
        // generator did not size Work for.
        auto clamped = value;
        if constexpr (Sig::phys_min != std::numeric_limits<typename Sig::Type>::min())
        {
            if (clamped < Sig::phys_min)
            {
                clamped = Sig::phys_min;
            }
        }
        if constexpr (Sig::phys_max != std::numeric_limits<typename Sig::Type>::max())
        {
            if (clamped > Sig::phys_max)
            {
                clamped = Sig::phys_max;
            }
        }

        if constexpr (Sig::identity)
        {
            return static_cast<typename Sig::Raw>(static_cast<acc_t<Sig>>(clamped) & field_mask<Sig>());
        }
        else
        {
            using Work = typename Sig::Work;
            const auto numerator = static_cast<Work>(static_cast<Work>(clamped) - Sig::offset);
            auto quotient = static_cast<Work>(numerator / Sig::scale);
            const auto remainder = static_cast<Work>(numerator % Sig::scale);
            const auto abs_remainder = (remainder < 0) ? static_cast<Work>(-remainder) : remainder;
            const auto abs_scale = (Sig::scale < 0) ? static_cast<Work>(-Sig::scale) : Sig::scale;
            if (abs_remainder >= static_cast<Work>(abs_scale - abs_remainder))
            {
                quotient = static_cast<Work>(quotient + (((numerator < 0) == (Sig::scale < 0)) ? 1 : -1));
            }
            return static_cast<typename Sig::Raw>(static_cast<acc_t<Sig>>(quotient) & field_mask<Sig>());
        }
    }
    else if constexpr (Sig::domain == value_domain::Float)
    {
        const float scaled = (value - Sig::offset) / Sig::scale;
        const int32_t rounded = round_saturate<float, int32_t>(scaled, Sig::raw_min, Sig::raw_max);
        return static_cast<typename Sig::Raw>(static_cast<acc_t<Sig>>(rounded) & field_mask<Sig>());
    }
    else if constexpr (Sig::domain == value_domain::Double)
    {
        const double scaled = (value - Sig::offset) / Sig::scale;
        const auto rounded = round_saturate<double, typename Sig::Conv>(scaled, Sig::raw_min, Sig::raw_max);
        return static_cast<typename Sig::Raw>(static_cast<acc_t<Sig>>(rounded) & field_mask<Sig>());
    }
    else if constexpr (Sig::domain == value_domain::IeeeFloat)
    {
        if constexpr (Sig::identity)
        {
            return static_cast<typename Sig::Raw>(std::bit_cast<uint32_t>(value));
        }
        else
        {
            return static_cast<typename Sig::Raw>(std::bit_cast<uint32_t>(static_cast<float>((value - Sig::offset) / Sig::scale)));
        }
    }
    else
    {
        if constexpr (Sig::identity)
        {
            return static_cast<typename Sig::Raw>(std::bit_cast<uint64_t>(value));
        }
        else
        {
            return static_cast<typename Sig::Raw>(std::bit_cast<uint64_t>((value - Sig::offset) / Sig::scale));
        }
    }
}

template <typename Sig>
constexpr typename Sig::Type decode_signal(std::span<const uint8_t> frame)
{
    return from_raw<Sig>(static_cast<typename Sig::Raw>(extract_bits<Sig>(frame)));
}

template <typename Sig>
constexpr void encode_signal(std::span<uint8_t> frame, typename Sig::Type value)
{
    insert_bits<Sig>(frame, to_raw<Sig>(value));
}

}  // namespace dbc_detail

// One signal's raw bits and value, either way. The signal's traits type is the
// tag, so these are found by argument-dependent lookup:
//
//     const auto raw = to_raw(Frame_t::sig_Speed_t{}, frame.Speed);
template <typename Sig>
constexpr typename Sig::Raw to_raw(Sig, typename Sig::Type value)
{
    return dbc_detail::to_raw<Sig>(value);
}

template <typename Sig>
constexpr typename Sig::Type from_raw(Sig, typename Sig::Raw raw)
{
    return dbc_detail::from_raw<Sig>(raw);
}

}  // namespace @BASE@

#endif  // @GUARD@
)CPP";

std::string replaceAll(std::string text, std::string_view from, std::string_view to)
{
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos)
    {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

} // namespace

void generate_cpp_common_header(const std::string &base, std::ostream &out)
{
    std::string baseUpper = base;
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(), ::toupper);

    std::string text = replaceAll(std::string(kCommonHeader), "@GUARD@", baseUpper + "_COMMON_H_");
    text = replaceAll(std::move(text), "@BASE@", base);
    out << text;
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
    fmt::print(hout, "    [[nodiscard]] constexpr Messages decode(uint32_t message_id, std::span<const uint8_t> data)\n");
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
