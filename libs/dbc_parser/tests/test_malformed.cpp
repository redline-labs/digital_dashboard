// Bad DBC files, and what the parser is supposed to say about each one.
//
// Every case here is a failure mode the old parser had: it either accepted the
// file and generated silently wrong code, or -- for the missing multiplexor --
// took the code generator down with a segfault part way through writing a
// header. The contract being pinned is that a file we do not fully understand
// is rejected with a line number, never half understood.

#include "dbc_parser/dbc_parser.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct Expectation
{
    std::string_view name;
    std::string_view source;
    // A fragment the diagnostics must contain. Empty means the file must parse.
    std::string_view expectedError;
};

constexpr std::string_view kPreamble = R"(VERSION "test"

BU_ : ECU

)";

const std::vector<Expectation> kCases = {
    {
        "well formed",
        R"(BO_ 100 Frame: 8 ECU
 SG_ Speed : 7|16@0+ (0.1,0) [0|500] "km/h" ECU
)",
        "",
    },
    {
        "multiplexed with no multiplexor",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A m0 : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ B m1 : 15|8@0+ (1,0) [0|255] "" ECU
)",
        "no multiplexor signal",
    },
    {
        "two multiplexors",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A M : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ B M : 15|8@0+ (1,0) [0|255] "" ECU
 SG_ C m0 : 23|8@0+ (1,0) [0|255] "" ECU
)",
        "more than one multiplexor",
    },
    {
        "signal runs past the end of the frame",
        R"(BO_ 100 Frame: 2 ECU
 SG_ A : 7|32@0+ (1,0) [0|255] "" ECU
)",
        "past the end of a 2 byte frame",
    },
    {
        "little endian signal past the end of the frame",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 56|16@1+ (1,0) [0|255] "" ECU
)",
        "past the end of a 8 byte frame",
    },
    {
        "duplicate message id",
        R"(BO_ 100 One: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
BO_ 100 Two: 8 ECU
 SG_ B : 7|8@0+ (1,0) [0|255] "" ECU
)",
        "defined by more than one BO_",
    },
    {
        "duplicate message name",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
BO_ 200 Frame: 8 ECU
 SG_ B : 7|8@0+ (1,0) [0|255] "" ECU
)",
        "used more than once",
    },
    {
        "duplicate signal name",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ A : 15|8@0+ (1,0) [0|255] "" ECU
)",
        "more than once",
    },
    {
        "signal named as a C++ keyword",
        R"(BO_ 100 Frame: 8 ECU
 SG_ class : 7|8@0+ (1,0) [0|255] "" ECU
)",
        "cannot be used as a C++ identifier",
    },
    {
        "zero scale would divide by zero on encode",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (0,0) [0|255] "" ECU
)",
        "scale of zero",
    },
    {
        "zero length signal",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|0@0+ (1,0) [0|255] "" ECU
)",
        "zero length",
    },
    {
        "signal wider than 64 bits",
        R"(BO_ 100 Frame: 16 ECU
 SG_ A : 7|72@0+ (1,0) [0|255] "" ECU
)",
        "64 bit words",
    },
    {
        "missing open paren on the scale",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ 1,0) [0|255] "" ECU
)",
        "expects '('",
    },
    {
        "missing pipe in the range",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0 255] "" ECU
)",
        "expects '|'",
    },
    {
        "byte order that is neither Intel nor Motorola",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@2+ (1,0) [0|255] "" ECU
)",
        "must be 0 (Motorola) or 1 (Intel)",
    },
    {
        "unterminated string",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "km/h ECU
)",
        "unterminated string",
    },
    {
        "VAL_ for a signal that does not exist",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
VAL_ 100 Nonexistent 0 "Off" 1 "On" ;
)",
        "does not define",
    },
    {
        "VAL_ naming a value table that was never defined",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
VAL_ 100 A MissingTable ;
)",
        "not defined by any VAL_TABLE_",
    },
    {
        "SIG_VALTYPE_ float on a signal that is not 32 bits",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|16@0+ (1,0) [0|255] "" ECU
SIG_VALTYPE_ 100 A : 1;
)",
        "not 32",
    },
    {
        // The point is the diagnostic, not the rejection: this used to fail
        // with "SG_ expects ':'" because the m0M token was not recognised and
        // was left in the stream, taking the whole message down with it.
        "nested multiplexing is named as such",
        R"(BO_ 100 Frame: 8 ECU
 SG_ Top M : 7|4@0+ (1,0) [0|15] "" ECU
 SG_ Inner m0M : 3|4@0+ (1,0) [0|15] "" ECU
 SG_ Leaf m1 : 15|8@0+ (1,0) [0|255] "" ECU
)",
        "nested multiplexing",
    },
    {
        "extended multiplexing is refused rather than mis-decoded",
        R"(BO_ 100 Frame: 8 ECU
 SG_ M0 M : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ A m0 : 15|8@0+ (1,0) [0|255] "" ECU
SG_MUL_VAL_ 100 A M0 1-2;
)",
        "extended multiplexing",
    },
    {
        "no messages at all",
        "this is not a DBC file\n",
        "no BO_ message definitions found",
    },
};

// Constructs that must parse, so the stricter rules above do not quietly
// start rejecting real files.
const std::vector<Expectation> kAcceptedCases = {
    {
        "ordinary flat multiplexing",
        R"(BO_ 100 Frame: 8 ECU
 SG_ Top M : 7|4@0+ (1,0) [0|15] "" ECU
 SG_ First m0 : 3|4@0+ (1,0) [0|15] "" ECU
 SG_ Leaf m1 : 15|8@0+ (1,0) [0|255] "" ECU
)",
        "",
    },
    {
        "named value table referenced by VAL_",
        R"(VAL_TABLE_ OnOff 1 "On" 0 "Off" ;
BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
VAL_ 100 A OnOff ;
)",
        "",
    },
    {
        "scientific notation in the scale",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1.5e-3,0) [0|255] "" ECU
)",
        "",
    },
    {
        "negative scale and offset",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0- (-0.5,-273.15) [-500|500] "C" ECU
)",
        "",
    },
    {
        "signal filling the whole frame",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 0|64@1+ (1,0) [0|0] "" ECU
)",
        "",
    },
    {
        "message with no signals",
        R"(BO_ 100 Frame: 8 ECU
)",
        "",
    },
    {
        "comment containing a quote",
        R"(BO_ 100 Frame: 8 ECU
 SG_ A : 7|8@0+ (1,0) [0|255] "" ECU
CM_ SG_ 100 A "he said \"go\" and left";
)",
        "",
    },
};

bool run(const Expectation &expectation, bool mustParse)
{
    const std::string source = std::string(kPreamble) + std::string(expectation.source);

    dbc_parser::Parser parser(source);
    const auto db = parser.parse();

    std::string combined;
    for (const auto &entry : parser.diagnostics().entries())
    {
        combined += entry.message;
        combined += '\n';
    }

    if (mustParse)
    {
        if (!db)
        {
            std::fprintf(stderr, "FAIL [%.*s]: expected this to parse, but it did not:\n%s",
                         static_cast<int>(expectation.name.size()), expectation.name.data(),
                         combined.c_str());
            return false;
        }
        return true;
    }

    if (db)
    {
        std::fprintf(stderr, "FAIL [%.*s]: expected a rejection, but the file was accepted\n",
                     static_cast<int>(expectation.name.size()), expectation.name.data());
        return false;
    }

    if (combined.find(expectation.expectedError) == std::string::npos)
    {
        std::fprintf(stderr, "FAIL [%.*s]: rejected, but no diagnostic mentions '%.*s'. Got:\n%s",
                     static_cast<int>(expectation.name.size()), expectation.name.data(),
                     static_cast<int>(expectation.expectedError.size()),
                     expectation.expectedError.data(), combined.c_str());
        return false;
    }

    // A rejection is only useful if it says where. Line 0 means the diagnostic
    // was raised without a position and the caller has nothing to go on.
    bool anyPositioned = false;
    for (const auto &entry : parser.diagnostics().entries())
    {
        if ((entry.severity == dbc_parser::Severity::Error) && (entry.line > 0))
        {
            anyPositioned = true;
        }
    }

    if (!anyPositioned)
    {
        std::fprintf(stderr, "FAIL [%.*s]: rejected without a line number\n",
                     static_cast<int>(expectation.name.size()), expectation.name.data());
        return false;
    }

    return true;
}

} // namespace

int main()
{
    int failures = 0;

    for (const auto &expectation : kCases)
    {
        if (!run(expectation, expectation.expectedError.empty()))
        {
            failures += 1;
        }
    }

    for (const auto &expectation : kAcceptedCases)
    {
        if (!run(expectation, true))
        {
            failures += 1;
        }
    }

    std::printf("malformed corpus: %zu rejections, %zu acceptances, %d failures\n",
                kCases.size(), kAcceptedCases.size(), failures);

    return (failures == 0) ? 0 : 1;
}
