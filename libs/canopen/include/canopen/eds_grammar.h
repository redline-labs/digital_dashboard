// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lexical half of reading an EDS: split the file into sections, and each
// section into lines, keeping a source position for every one of them.
//
// Nothing here knows what an object dictionary is. That is deliberate. The
// previous version of this grammar decided in the parse rules which sections
// were objects and which were not, which meant a section it could not classify
// -- `[1A00]`, because `A` is not a decimal digit -- fell through to a silent
// catch-all and vanished, taking TPDO1's entire mapping with it. A grammar that
// only recognises "a bracketed name followed by lines" has nothing to fall
// through to: every section reaches the caller, and a name that makes no sense
// becomes a diagnostic in eds_parser.cpp rather than a missing object.
//
// Positions are carried as raw iterators into the input. parse_eds() turns them
// into line and column numbers once, at the end, using lexy's input location
// machinery -- so the cost is paid per diagnostic rather than per line.
#ifndef CANOPEN_EDS_GRAMMAR_H
#define CANOPEN_EDS_GRAMMAR_H

#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>

#include <string>
#include <vector>

namespace canopen
{
namespace grammar
{

namespace dsl = lexy::dsl;

// One line of a section body, verbatim and untrimmed. Comments, blank lines and
// malformed lines all arrive here looking the same; telling them apart is the
// reader's job, and it needs `position` to say where.
struct RawLine
{
    std::string text;
    const void* position { nullptr };
};

struct RawSection
{
    // The text between the brackets, exactly as written.
    std::string name;
    const void* position { nullptr };
    std::vector<RawLine> lines;
};

using RawDocument = std::vector<RawSection>;

// Everything on a line except its terminator. `dsl::ascii::newline` covers both
// CR and LF, so a CRLF file leaves the position on the CR and `dsl::eol` below
// consumes the pair.
//
// Matching a code point rather than an ASCII character matters: EDS files are
// nominally ASCII but vendors put accented names and copyright signs in
// Description and ParameterName, and rejecting those would fail the whole file
// over a byte nobody reads.
static constexpr auto line_content = dsl::code_point - dsl::ascii::newline;

struct line_text : lexy::token_production
{
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_(line_content)));
    static constexpr auto value = lexy::as_string<std::string>;
};

struct section_name : lexy::token_production
{
    static constexpr auto rule
        = dsl::capture(dsl::token(dsl::while_(line_content - dsl::lit_c<']'>)));
    static constexpr auto value = lexy::as_string<std::string>;
};

struct body_line
{
    static constexpr auto rule = dsl::position + dsl::p<line_text>;
    static constexpr auto value = lexy::callback<RawLine>(
        [](const void* position, std::string&& text)
        {
            return RawLine { std::move(text), position };
        });
};

// Body lines run until the next section header or the end of the file. The
// terminator is only ever tested at the start of a line, so a `[` appearing
// mid-line is ordinary text.
struct body_lines
{
    static constexpr auto rule
        = dsl::terminator(dsl::peek(dsl::lit_c<'['>) | dsl::eof)
              .opt_list(dsl::p<body_line>, dsl::trailing_sep(dsl::newline));
    static constexpr auto value = lexy::as_list<std::vector<RawLine>>;
};

struct section
{
    static constexpr auto rule = dsl::position + dsl::lit_c<'['> + dsl::p<section_name>
        + dsl::lit_c<']'>
        // Anything after the closing bracket is tolerated and ignored; some
        // tools write a trailing comment there.
        + dsl::while_(line_content) + dsl::eol + dsl::p<body_lines>;
    static constexpr auto value = lexy::callback<RawSection>(
        [](const void* position, std::string&& name, std::vector<RawLine>&& lines)
        {
            return RawSection { std::move(name), position, std::move(lines) };
        },
        [](const void* position, std::string&& name, lexy::nullopt)
        {
            return RawSection { std::move(name), position, {} };
        });
};

struct document
{
    // Leading blank lines belong to no section, so they are skipped here rather
    // than becoming a phantom section body.
    static constexpr auto rule
        = dsl::while_(dsl::ascii::space) + dsl::terminator(dsl::eof).opt_list(dsl::p<section>);
    static constexpr auto value = lexy::as_list<RawDocument>;
};

} // namespace grammar
} // namespace canopen

#endif // CANOPEN_EDS_GRAMMAR_H
