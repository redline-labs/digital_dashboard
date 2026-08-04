// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading an EDS, and saying what is wrong with it.
//
// Parsing and validation are separate calls on purpose. parse_eds() answers
// "what does this file say", and reports only what stopped it reading a line.
// validate() answers "is what it says coherent" -- sub0 against the real sub
// count, declared objects against present ones, PDO mappings against the 64-bit
// limit, defaults against their own limits. A file can parse cleanly and still
// be wrong in every one of those ways, which is exactly the failure mode that
// let a decimal-radix bug survive in this library: everything parsed, nothing
// was where it belonged, and the code generator quietly fell back to constants.
#ifndef CANOPEN_EDS_PARSER_H
#define CANOPEN_EDS_PARSER_H

#include "canopen/eds_ast.h"

#include <string>
#include <string_view>
#include <vector>

namespace canopen
{

enum class Severity
{
    // The file says something impossible or self-contradictory. Anything
    // derived from it is suspect.
    Error,
    // The file is readable but something in it is unusual enough to be worth
    // saying out loud -- an unknown data type, a key we do not model.
    Warning,
};

struct Diagnostic
{
    Severity severity { Severity::Error };
    // 1-based, 0 when the diagnostic is about the file as a whole rather than a
    // particular place in it.
    int line { 0 };
    int column { 0 };
    std::string message;
};

// Formats as "12:5: error: ..." -- the shape editors already know how to jump
// to, and the shape the code generator prints under --strict.
std::string to_string(const Diagnostic& diagnostic);

struct ParseResult
{
    ObjectDictionary od;
    std::vector<Diagnostic> diagnostics;

    // Whether anything was reported at Error severity. Note that this is not
    // "the parse produced nothing" -- a malformed line is skipped and the rest
    // of the file is still read, so a result can be both unusable as a whole
    // and useful for finding out why.
    bool ok() const;
};

// Read an EDS. Never throws and never returns nothing: a file that fails
// wholesale comes back as an empty dictionary plus the diagnostics explaining
// it, which is more useful to a caller than std::nullopt.
ParseResult parse_eds(std::string_view text);

// Semantic checks over an already-parsed dictionary. Returns an empty vector
// when the file is coherent.
std::vector<Diagnostic> validate(const ObjectDictionary& od);

} // namespace canopen

#endif // CANOPEN_EDS_PARSER_H
