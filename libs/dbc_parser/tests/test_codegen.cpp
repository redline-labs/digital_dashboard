// The shape of the generated text, driven from DBCs held in string literals.
//
// Generation is a pure function of the parsed database, so none of this needs
// a file, a build step or a compiler -- which is what makes these properties
// testable at all. They were not covered by anything before: the differential
// test compares numbers, so it says nothing about how a signal is *named* or
// whether a comment was escaped on its way into a string literal.
//
// Type selection is checked one signal at a time. The version this replaced
// searched the whole output for `using Type = int64_t;`, so a check passed as
// long as ANY signal anywhere had that type.

#include "dbc_parser/dbc_parser.h"
#include "dbc_parser/generate_h.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

int failures = 0;

constexpr std::string_view kPreamble = R"(VERSION "test"

BU_ : ECU

)";

// Generates from an embedded DBC and returns every file concatenated, which is
// all these assertions need.
std::string generate(std::string_view body)
{
    const std::string source = std::string(kPreamble) + std::string(body);

    dbc_parser::Parser parser(source);
    const auto db = parser.parse();
    if (!db)
    {
        std::string message = "expected this to parse:\n";
        for (const auto &entry : parser.diagnostics().entries())
        {
            message += "  " + entry.message + "\n";
        }
        std::fprintf(stderr, "FAIL: %s", message.c_str());
        failures += 1;
        return {};
    }

    std::string all;
    for (const auto &file : dbc_codegen::generate_sources(*db, "t"))
    {
        all += file.content;
    }
    return all;
}

void fail(std::string_view what, std::string_view detail)
{
    std::fprintf(stderr, "FAIL [%.*s]: %.*s\n", static_cast<int>(what.size()), what.data(),
                 static_cast<int>(detail.size()), detail.data());
    failures += 1;
}

void expectContains(std::string_view what, const std::string &haystack, std::string_view needle)
{
    if (haystack.find(needle) == std::string::npos)
    {
        fail(what, "generated code does not contain '" + std::string(needle) + "'");
    }
}

void expectAbsent(std::string_view what, const std::string &haystack, std::string_view needle)
{
    if (haystack.find(needle) != std::string::npos)
    {
        fail(what, "generated code should not contain '" + std::string(needle) + "'");
    }
}

// The traits struct of exactly one signal. Fails if the signal is missing or
// defined twice, so a check can never land on a neighbour.
std::string signalBlock(const std::string &all, std::string_view name)
{
    const std::string open = "    struct sig_" + std::string(name) + "_t\n    {\n";
    const size_t begin = all.find(open);
    if ((begin == std::string::npos) || (all.find(open, begin + 1) != std::string::npos))
    {
        fail(name, "signal traits missing or defined more than once");
        return {};
    }
    const size_t end = all.find("\n    };\n", begin);
    return all.substr(begin, end - begin);
}

// A whole line of one signal's traits, so `int16_t` cannot match `uint16_t`.
void expectTrait(std::string_view what, const std::string &block, std::string_view line)
{
    expectContains(what, block, "\n        " + std::string(line) + "\n");
}

void testStringEscaping()
{
    // The lexer strips the DBC's own escaping, so a quote arrives here bare.
    // Emitted as-is it would close the string literal early and let a vendor
    // file decide what our build compiles.
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
CM_ SG_ 100 A "he said \"go\" then left\\";
)");

    expectContains("escaped quote", out, R"(comment = "he said \"go\" then left\\")");
    expectAbsent("no bare quote", out, R"(comment = "he said "go")");
}

void testUnitEscaping()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "in\"" ECU
)");

    expectContains("escaped unit", out, R"(unit = "in\"")");
}

void testEnumeratorNaming()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ Plain : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ Awkward : 15|8@0+ (1,0) [0|255] "" ECU
 SG_ Colliding : 23|8@0+ (1,0) [0|255] "" ECU
VAL_ 100 Plain 0 "Off" 1 "On" ;
VAL_ 100 Awkward 0 "0 to 100%" 1 "" 2 "class" 3 "---" ;
VAL_ 100 Colliding 0 "Fault-A" 1 "Fault_A" ;
)");

    // Unique names stay bare. Every enumerator used to be suffixed with its own
    // raw value, because the dedup loop compared each entry against itself.
    expectContains("bare enumerator", out, "Off = 0,");
    expectContains("bare enumerator", out, "On = 1,");
    expectAbsent("no gratuitous suffix", out, "Off_0");
    expectAbsent("no gratuitous suffix", out, "On_1");

    // A name that would not be an identifier is repaired rather than emitted.
    expectContains("leading digit repaired", out, "_0_to_100 = 0,");
    expectContains("empty description replaced", out, "Value = 1,");
    // A reserved word is still a reserved word once sanitised, so it has to be
    // kept out of enumerator position too.
    expectAbsent("reserved word not emitted bare", out, "            class = 2,");
    expectContains("punctuation-only replaced", out, "= 3,");

    // Names that genuinely collide after sanitising do get disambiguated.
    expectContains("collision disambiguated", out, "Fault_A = 0,");
    expectContains("collision disambiguated", out, "Fault_A_1 = 1,");
}

void testBoolAndEnum()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ Flag : 0|1@1+ (1,0) [0|1] "" ECU
 SG_ SignedBit : 1|1@1- (1,0) [-1|0] "" ECU
 SG_ FlagTable : 2|1@1+ (1,0) [0|1] "" ECU
 SG_ SmallTable : 3|3@1- (1,0) [-4|3] "" ECU
 SG_ WideTable : 8|8@1+ (1,0) [0|255] "" ECU
VAL_ 100 FlagTable 0 "Ok" 1 "Fault" ;
VAL_ 100 SmallTable -4 "Low" 3 "High" ;
VAL_ 100 WideTable 300 "Beyond" 0 "Zero" ;
)");

    const std::string flag = signalBlock(out, "Flag");
    expectTrait("one unsigned bit is a bool", flag, "using Type = bool;");
    expectTrait("one unsigned bit is a bool", flag, "static constexpr value_domain domain = value_domain::Bool;");
    expectTrait("one bit fits a byte", flag, "using Raw = uint8_t;");

    const std::string signedBit = signalBlock(out, "SignedBit");
    expectTrait("a signed bit is -1 or 0, not a bool", signedBit, "using Type = int8_t;");

    // A value table keeps its names even on a single bit.
    expectTrait("a one bit table stays an enum", signalBlock(out, "FlagTable"),
                "enum class Values : uint8_t");
    expectTrait("a negative entry makes the base signed", signalBlock(out, "SmallTable"),
                "enum class Values : int8_t");
    expectTrait("an entry wider than the field widens the base", signalBlock(out, "WideTable"),
                "enum class Values : uint16_t");
}

void testIntegerDomain()
{
    const std::string out = generate(R"(BO_ 100 Coolant: 8 ECU
 SG_ Coolant : 0|8@1+ (1,-40) [-40|215] "C" ECU
BO_ 101 Thousand: 8 ECU
 SG_ Thousand : 0|8@1+ (1000,0) [0|255000] "" ECU
BO_ 102 Negated: 8 ECU
 SG_ Negated : 0|8@1+ (-1,0) [-255|0] "" ECU
BO_ 103 SignedNegated: 8 ECU
 SG_ SignedNegated : 0|16@1- (-1,0) [-32767|32768] "" ECU
BO_ 104 Twelve: 8 ECU
 SG_ Twelve : 0|12@1+ (1,0) [0|4095] "" ECU
BO_ 105 Wide: 8 ECU
 SG_ Wide : 0|32@1+ (1,-40) [-40|4294967255] "" ECU
BO_ 106 FullUnsigned: 8 ECU
 SG_ FullUnsigned : 0|64@1+ (1,0) [0|0] "" ECU
BO_ 107 FullSigned: 8 ECU
 SG_ FullSigned : 0|64@1- (1,0) [0|0] "" ECU
)");

    // The bug the old rule was written for: an unsigned field with a negative
    // offset produces negative values. -40..215 needs sixteen bits, not 64.
    const std::string coolant = signalBlock(out, "Coolant");
    expectTrait("negative offset gives a signed type", coolant, "using Type = int16_t;");
    expectTrait("integral scaling is integer arithmetic", coolant,
                "static constexpr value_domain domain = value_domain::Integer;");
    expectTrait("integer arithmetic in 32 bits", coolant, "using Work = int32_t;");
    expectTrait("offset is an integer constant", coolant, "static constexpr int32_t offset = -40;");
    expectTrait("range from the raw field", coolant, "static constexpr int16_t phys_min = -40;");
    expectTrait("range from the raw field", coolant, "static constexpr int16_t phys_max = 215;");

    expectTrait("255000 needs 32 bits", signalBlock(out, "Thousand"), "using Type = uint32_t;");
    expectTrait("negative scale gives a signed type", signalBlock(out, "Negated"), "using Type = int16_t;");
    expectTrait("-32767..32768 does not fit 16 bits", signalBlock(out, "SignedNegated"),
                "using Type = int32_t;");

    const std::string twelve = signalBlock(out, "Twelve");
    expectTrait("unscaled 12 bits is a uint16_t", twelve, "using Type = uint16_t;");
    expectAbsent("unscaled needs no arithmetic type", twelve, "using Work");

    const std::string wide = signalBlock(out, "Wide");
    expectTrait("a 32 bit field with an offset needs 64", wide, "using Type = int64_t;");
    expectTrait("and so does its arithmetic", wide, "using Work = int64_t;");

    expectTrait("64 bit unsigned", signalBlock(out, "FullUnsigned"),
                "static constexpr uint64_t phys_max = 18446744073709551615ULL;");
    expectTrait("INT64_MIN cannot be written as a plain literal", signalBlock(out, "FullSigned"),
                "static constexpr int64_t phys_min = (-9223372036854775807LL - 1);");
}

void testDoubleFallback()
{
    const std::string out = generate(R"(BO_ 100 Doubled: 8 ECU
 SG_ Doubled : 0|64@1+ (2,0) [0|0] "" ECU
BO_ 101 Shifted: 8 ECU
 SG_ Shifted : 0|64@1+ (1,1) [0|0] "" ECU
)");

    // Integral, but 2^65 and 2^64 do not fit any 64 bit type.
    const std::string doubled = signalBlock(out, "Doubled");
    expectTrait("too wide for integers", doubled, "using Type = double;");
    expectTrait("too wide for integers", doubled, "static constexpr value_domain domain = value_domain::Double;");
    expectTrait("rounds into the field's own width", doubled, "using Conv = uint64_t;");
    expectTrait("one past uint64_t is a double too", signalBlock(out, "Shifted"), "using Type = double;");
}

void testFloatThreshold()
{
    // Each ratio of offset to scale is exact in binary, so which side of 2^20 a
    // signal lands on does not depend on how the generator rounds.
    const std::string out = generate(R"(BO_ 100 Tenth: 8 ECU
 SG_ Tenth : 0|20@1+ (0.1,0) [0|0] "" ECU
BO_ 101 HalfHalf: 8 ECU
 SG_ HalfHalf : 0|20@1+ (0.5,0.5) [0|0] "" ECU
BO_ 102 SignedTenth: 8 ECU
 SG_ SignedTenth : 0|21@1- (0.1,0) [0|0] "" ECU
BO_ 103 JustBelow: 8 ECU
 SG_ JustBelow : 0|12@1+ (0.25,261120) [0|0] "" ECU
BO_ 104 JustAt: 8 ECU
 SG_ JustAt : 0|12@1+ (0.25,261120.25) [0|0] "" ECU
BO_ 105 Kelvin: 8 ECU
 SG_ Kelvin : 0|8@1+ (0.5,-273.15) [0|0] "" ECU
BO_ 106 Tiny: 8 ECU
 SG_ Tiny : 0|8@1+ (1e-40,0) [0|0] "" ECU
)");

    const std::string tenth = signalBlock(out, "Tenth");
    expectTrait("2^20 - 1 steps is float", tenth, "using Type = float;");
    expectTrait("2^20 - 1 steps is float", tenth, "static constexpr value_domain domain = value_domain::Float;");
    expectTrait("float constants are float literals", tenth, "static constexpr float scale = 0.100000001f;");
    expectTrait("float constants are float literals", tenth, "static constexpr float offset = 0.0f;");
    expectTrait("rounded through int32_t", tenth, "using Conv = int32_t;");
    expectTrait("saturates to the field", tenth, "static constexpr int32_t raw_max = 1048575;");

    expectTrait("the offset counts: exactly 2^20 is double", signalBlock(out, "HalfHalf"),
                "using Type = double;");
    expectTrait("a signed 21 bit field reaches 2^20", signalBlock(out, "SignedTenth"),
                "using Type = double;");
    expectTrait("2^20 - 1 steps via the offset is float", signalBlock(out, "JustBelow"),
                "using Type = float;");
    expectTrait("2^20 steps via the offset is double", signalBlock(out, "JustAt"),
                "using Type = double;");
    expectTrait("offset spelled as its float", signalBlock(out, "Kelvin"),
                "static constexpr float offset = -273.149994f;");

    // A scale that is zero or subnormal as a float would decode everything
    // as nothing.
    expectTrait("a scale float cannot hold is double", signalBlock(out, "Tiny"), "using Type = double;");
}

void testValueTableWithScalingIsNotAnEnum()
{
    // Enumerators are raw values. With a scale in play the field would hold a
    // scaled number while the enumerators named unscaled ones, and the two
    // would silently disagree -- so this must not become an enum.
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ Scaled : 7|8@0+ (0.1,0) [0|25] "" ECU
VAL_ 100 Scaled 0 "Off" 1 "On" ;
)");

    expectAbsent("scaled value table is not an enum", out, "enum class Values");
    expectTrait("scaled value table decodes as a number", signalBlock(out, "Scaled"), "using Type = float;");
}

void testIeeeSignals()
{
    const std::string out = generate(R"(BO_ 100 AsFloat: 8 ECU
 SG_ AsFloat : 0|32@1- (1,0) [0|0] "" ECU
BO_ 101 ScaledFloat: 8 ECU
 SG_ ScaledFloat : 0|32@1- (0.5,10) [0|0] "" ECU
BO_ 102 AsDouble: 8 ECU
 SG_ AsDouble : 0|64@1- (1,0) [0|0] "" ECU
SIG_VALTYPE_ 100 AsFloat : 1;
SIG_VALTYPE_ 101 ScaledFloat : 1;
SIG_VALTYPE_ 102 AsDouble : 2;
)");

    const std::string asFloat = signalBlock(out, "AsFloat");
    expectTrait("float on the wire is a float", asFloat, "using Type = float;");
    expectTrait("float on the wire is a float", asFloat,
                "static constexpr value_domain domain = value_domain::IeeeFloat;");
    expectTrait("float on the wire is 32 raw bits", asFloat, "using Raw = uint32_t;");

    // Scaling a float32 by an arbitrary factor can leave float32's range.
    expectTrait("scaled float on the wire is a double", signalBlock(out, "ScaledFloat"),
                "using Type = double;");

    const std::string asDouble = signalBlock(out, "AsDouble");
    expectTrait("double on the wire", asDouble, "using Type = double;");
    expectTrait("double on the wire", asDouble,
                "static constexpr value_domain domain = value_domain::IeeeDouble;");
    expectTrait("double on the wire is 64 raw bits", asDouble, "using Raw = uint64_t;");
}

void testRawWidths()
{
    const std::string out = generate(R"(BO_ 100 L1: 8 ECU
 SG_ L1 : 0|1@1+ (1,0) [0|1] "" ECU
BO_ 101 L8: 8 ECU
 SG_ L8 : 0|8@1+ (1,0) [0|0] "" ECU
BO_ 102 L9: 8 ECU
 SG_ L9 : 0|9@1+ (1,0) [0|0] "" ECU
BO_ 103 L16: 8 ECU
 SG_ L16 : 0|16@1+ (1,0) [0|0] "" ECU
BO_ 104 L17: 8 ECU
 SG_ L17 : 0|17@1+ (1,0) [0|0] "" ECU
BO_ 105 L32: 8 ECU
 SG_ L32 : 0|32@1+ (1,0) [0|0] "" ECU
BO_ 106 L33: 8 ECU
 SG_ L33 : 0|33@1+ (1,0) [0|0] "" ECU
BO_ 107 L64: 8 ECU
 SG_ L64 : 0|64@1+ (1,0) [0|0] "" ECU
)");

    expectTrait("1 bit", signalBlock(out, "L1"), "using Raw = uint8_t;");
    expectTrait("8 bits", signalBlock(out, "L8"), "using Raw = uint8_t;");
    expectTrait("9 bits", signalBlock(out, "L9"), "using Raw = uint16_t;");
    expectTrait("16 bits", signalBlock(out, "L16"), "using Raw = uint16_t;");
    expectTrait("17 bits", signalBlock(out, "L17"), "using Raw = uint32_t;");
    expectTrait("32 bits", signalBlock(out, "L32"), "using Raw = uint32_t;");
    expectTrait("33 bits", signalBlock(out, "L33"), "using Raw = uint64_t;");
    expectTrait("64 bits", signalBlock(out, "L64"), "using Raw = uint64_t;");
}

void testExtendedIdentifier()
{
    // A DBC spells a 29 bit id by setting bit 31. Carrying that through would
    // compare against a driver id that never has it set, and match nothing.
    // 2566843904 is 0x98FEEE00: the J1939 id 0x18FEEE00 with bit 31 set.
    const std::string out = generate(R"(BO_ 2566843904 Extended: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
)");

    expectContains("flag stripped from id", out, "static constexpr uint32_t id = 0x18FEEE00u;");
    expectContains("extended flag recorded", out, "static constexpr bool is_extended = true;");
}

void testDispatchIsASwitch()
{
    const std::string out = generate(R"(BO_ 100 One: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
BO_ 200 Two: 8 ECU
 SG_ B : 7|8@0+ (1,0) [0|255] "" ECU
)");

    // A linear else-if chain over every id was scanned per frame; the switch
    // gives the compiler something it can turn into a jump table.
    expectContains("switch dispatch", out, "switch (message_id)");
    expectAbsent("no else-if chain", out, "else if (message_id ==");
}

void testMultiplexGatesOnRawBits()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ Top M : 0|4@1+ (1,0) [0|15] "" ECU
 SG_ Leaf m2 : 8|8@1+ (1,0) [0|255] "" ECU
)");

    // The raw bits, in the accumulator word: compared in the multiplexor's own
    // uint8_t, a group index it cannot hold is a comparison clang rejects.
    expectContains("gate on raw bits", out,
                   "const dbc_detail::acc_t<sig_Top_t> mux_raw = dbc_detail::extract_bits<sig_Top_t>(frame);");
    expectContains("gate on raw bits", out, "if (mux_raw == 2u)");
    expectAbsent("not on the decoded value", out, "static_cast<uint64_t>(Top)");
}

void testLocalsDoNotShadowSignals()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ frame : 0|8@1+ (1,0) [0|255] "" ECU
 SG_ view : 8|8@1+ (1,0) [0|255] "" ECU
 SG_ fn : 16|8@1+ (1,0) [0|255] "" ECU
)");

    expectContains("decode parameter renamed", out, "decode(std::span<const uint8_t> frame_)");
    expectContains("encode span renamed", out, "const std::span<uint8_t> view_{frame_};");
    expectContains("visitor renamed", out, "visit(Func&& fn_)");
}

void testHelpersAreShared()
{
    const std::string out = generate(R"(BO_ 100 One: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
BO_ 200 Two: 8 ECU
 SG_ B : 7|8@0+ (1,0) [0|255] "" ECU
)");

    // Once per database, at namespace scope, instead of once per message
    // struct -- where every helper's locals could shadow a signal.
    expectContains("shared helpers", out, "namespace dbc_detail");
    expectAbsent("no per-message helpers", out, "static constexpr uint64_t extract_bits");
    expectAbsent("no double-only rounding", out, "round_half_away_from_zero");
}

} // namespace

// The file's declared [min|max]: emitted in the signal's own type, and railing
// the value only when the file actually set one. Half the signals in the wild
// declare [0|0], and those must stay inert.
void testDeclaredRange()
{
    const std::string out = generate(R"(BO_ 200 Ranged: 8 ECU
 SG_ Ranged : 0|8@1+ (1,0) [10|200] "" ECU
BO_ 201 Unset: 8 ECU
 SG_ Unset : 0|8@1+ (1,0) [0|0] "" ECU
BO_ 202 Offsetted: 8 ECU
 SG_ Offsetted : 0|8@1+ (1,-40) [-20|100] "C" ECU
BO_ 203 Scaled: 8 ECU
 SG_ Scaled : 0|16@1+ (0.1,0) [0|1000] "" ECU
BO_ 204 Flag: 8 ECU
 SG_ Flag : 0|1@1+ (1,0) [0|1] "" ECU
BO_ 205 Ieee: 8 ECU
 SG_ Ieee : 0|32@1- (1,0) [-2147483648|2147483647] "" ECU
SIG_VALTYPE_ 205 Ieee : 1;
)");

    // An integer signal's limits are its own type, not double, so railing it
    // never goes through floating point.
    const std::string ranged = signalBlock(out, "Ranged");
    expectTrait("declared range takes the signal's type", ranged, "using Type = uint8_t;");
    expectTrait("declared minimum is typed", ranged, "static constexpr uint8_t minimum = 10;");
    expectTrait("declared maximum is typed", ranged, "static constexpr uint8_t maximum = 200;");
    expectTrait("a real declaration rails", ranged, "static constexpr bool has_range = true;");

    // [0|0] is "unset" in about half the files. It must not rail everything to
    // zero, which is the whole reason the gate exists.
    const std::string unset = signalBlock(out, "Unset");
    expectTrait("an unset range is still typed", unset, "static constexpr uint8_t minimum = 0;");
    expectTrait("an unset range does not rail", unset, "static constexpr bool has_range = false;");

    // A negative offset makes the type signed, and the limits follow it.
    const std::string offsetted = signalBlock(out, "Offsetted");
    expectTrait("negative offset gives a signed type", offsetted, "using Type = int16_t;");
    expectTrait("declared minimum follows the signed type", offsetted,
                "static constexpr int16_t minimum = -20;");
    expectTrait("declared maximum follows the signed type", offsetted,
                "static constexpr int16_t maximum = 100;");

    // A float signal's limits are float, so the rail boundary is a value the
    // signal can actually take rather than one between two of them.
    const std::string scaled = signalBlock(out, "Scaled");
    expectTrait("a fractional scale is a float signal", scaled, "using Type = float;");
    expectTrait("declared minimum is a float literal", scaled, "static constexpr float minimum = 0.0f;");
    expectTrait("declared maximum is a float literal", scaled, "static constexpr float maximum = 1000.0f;");
    expectTrait("a real declaration rails", scaled, "static constexpr bool has_range = true;");

    // A bool cannot leave its range, so it is never railed however the file
    // declares it.
    const std::string flag = signalBlock(out, "Flag");
    expectTrait("one unsigned bit is a bool", flag, "using Type = bool;");
    expectTrait("a bool keeps double limits", flag, "static constexpr double minimum = 0.0;");
    expectTrait("a bool is never railed", flag, "static constexpr bool has_range = false;");

    // A SIG_VALTYPE_ signal's declared range describes its raw field, not its
    // value: this one says [-2147483648|2147483647], the int32 span of its 32
    // bits, while the value is an IEEE float running to 3.4e38. Railing to that
    // would destroy every large reading, so IEEE is never railed.
    const std::string ieee = signalBlock(out, "Ieee");
    expectTrait("SIG_VALTYPE_ 1 is an IEEE float", ieee, "using Type = float;");
    expectTrait("an IEEE signal is never railed", ieee, "static constexpr bool has_range = false;");

    // The rail itself, once, in the shared detail header.
    expectContains("the rail is emitted", out, "constexpr typename Sig::Type rail(typename Sig::Type value)");
    expectContains("decode rails", out, "return rail<Sig>(from_raw_unrailed<Sig>(raw));");
    expectContains("encode rails", out, "value = rail<Sig>(value);");
}

int main()
{
    testStringEscaping();
    testUnitEscaping();
    testEnumeratorNaming();
    testBoolAndEnum();
    testIntegerDomain();
    testDoubleFallback();
    testFloatThreshold();
    testValueTableWithScalingIsNotAnEnum();
    testIeeeSignals();
    testRawWidths();
    testExtendedIdentifier();
    testDispatchIsASwitch();
    testMultiplexGatesOnRawBits();
    testLocalsDoNotShadowSignals();
    testHelpersAreShared();
    testDeclaredRange();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("codegen text: all checks passed\n");
    return 0;
}
