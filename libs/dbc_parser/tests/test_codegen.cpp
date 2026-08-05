// The shape of the generated text, driven from DBCs held in string literals.
//
// Generation is a pure function of the parsed database, so none of this needs
// a file, a build step or a compiler -- which is what makes these properties
// testable at all. They were not covered by anything before: the differential
// test compares numbers, so it says nothing about how a signal is *named* or
// whether a comment was escaped on its way into a string literal.

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

void expectContains(std::string_view what, const std::string &haystack, std::string_view needle)
{
    if (haystack.find(needle) == std::string::npos)
    {
        std::fprintf(stderr, "FAIL [%.*s]: generated code does not contain '%.*s'\n",
                     static_cast<int>(what.size()), what.data(),
                     static_cast<int>(needle.size()), needle.data());
        failures += 1;
    }
}

void expectAbsent(std::string_view what, const std::string &haystack, std::string_view needle)
{
    if (haystack.find(needle) != std::string::npos)
    {
        std::fprintf(stderr, "FAIL [%.*s]: generated code should not contain '%.*s'\n",
                     static_cast<int>(what.size()), what.data(),
                     static_cast<int>(needle.size()), needle.data());
        failures += 1;
    }
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

void testSignalTypeSelection()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ UnsignedNegOffset : 7|8@0+ (1,-40) [-40|215] "C" ECU
 SG_ UnsignedZeroOffset : 15|8@0+ (1,0) [0|255] "" ECU
 SG_ SignedIdentity : 23|8@0- (1,0) [-128|127] "" ECU
 SG_ Fractional : 31|8@0+ (0.5,0) [0|127] "" ECU
 SG_ NegativeScale : 39|8@0+ (-1,0) [-255|0] "" ECU
)");

    // The bug this pins: an unsigned field with a negative offset produces
    // negative physical values, so handing it an unsigned type converted out
    // of range on every reading below the offset.
    expectContains("negative offset forces signed", out,
                   "static constexpr std::string_view name = \"UnsignedNegOffset\";");
    expectContains("negative offset forces signed", out, "using Type = int64_t;");
    expectContains("negative scale forces signed", out, "using Type = int64_t;");
    expectContains("plain unsigned stays unsigned", out, "using Type = uint64_t;");
    expectContains("fractional scale becomes double", out, "using Type = double;");
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
    expectContains("scaled value table decodes as a number", out, "using Type = double;");
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

void testFloatSignal()
{
    const std::string out = generate(R"(BO_ 100 Frame: 8 ECU
 SG_ A : 0|32@1- (1,0) [-1000|1000] "" ECU
SIG_VALTYPE_ 100 A : 1;
)");

    expectContains("float encoding recorded", out, "encoding = raw_encoding::Float;");
    expectContains("float decodes as double", out, "using Type = double;");
}

} // namespace

int main()
{
    testStringEscaping();
    testUnitEscaping();
    testEnumeratorNaming();
    testSignalTypeSelection();
    testValueTableWithScalingIsNotAnEnum();
    testExtendedIdentifier();
    testDispatchIsASwitch();
    testFloatSignal();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("codegen text: all checks passed\n");
    return 0;
}
