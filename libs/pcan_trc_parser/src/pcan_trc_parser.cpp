#include "pcan_trc_parser/pcan_trc_parser.h"

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/callback/bind.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy_ext/report_error.hpp>

#include <charconv>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <sstream>
#include <iostream>

namespace pcan_trc_parser
{

namespace
{
namespace dsl = lexy::dsl;

struct payload_list
{
    // Collect hex bytes until end-of-line (LF) or EOF
    static constexpr auto rule =
        dsl::terminator(dsl::newline | dsl::eof).opt_list(dsl::integer<std::uint8_t, dsl::hex>);
    static constexpr auto value = lexy::as_list<std::vector<std::uint8_t>>;
};

struct line_prod
{
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule =
        dsl::integer<std::uint64_t>                           // message number
        + dsl::capture(dsl::token(dsl::while_one(dsl::ascii::character - dsl::ascii::blank))) // timestamp
        + dsl::token(dsl::while_one(dsl::ascii::alpha))       // type token, ignored
        + dsl::integer<std::uint32_t, dsl::hex>               // id
        + dsl::capture(dsl::token(LEXY_LIT("Rx") | LEXY_LIT("Tx"))) // Rx/Tx
        + dsl::integer<std::uint8_t>                          // dlc
        + dsl::p<payload_list>;                               // payload bytes (ends at EOL/EOF)

    static constexpr auto value = lexy::callback<helpers::CanFrame>(
        [](std::uint64_t /*num*/, auto /*ts_lex*/, std::uint32_t id, auto /*dir_lex*/, std::uint8_t dlc, std::vector<std::uint8_t>&& bytes) {
            helpers::CanFrame f{};
            
            // Make sure the number of bytes matches the dlc, and also that its smaller
            // than our max frame size.
            size_t num_bytes = std::min(static_cast<size_t>(dlc), bytes.size());
            num_bytes = std::min(num_bytes, f.data.size());

            f.id = id;
            f.len = num_bytes;

            std::copy(bytes.begin(), bytes.begin() + num_bytes, f.data.begin());

            return f;
        }
    );
};

// Comment/header line starting with ';' (consume until LF)
struct comment_line
{
    static constexpr auto rule = LEXY_LIT(";") + dsl::until(dsl::newline) + dsl::opt(dsl::newline);
    static constexpr auto value = lexy::noop;
};

// Blank line (CRLF or LF)
struct blank_line
{
    static constexpr auto rule = dsl::opt(LEXY_LIT("\r")) + dsl::newline;
    static constexpr auto value = lexy::noop;
};

// A frame line wrapped as optional
struct frame_line
{
    static constexpr auto rule = dsl::p<line_prod> + dsl::opt(LEXY_LIT("\r")) + dsl::opt(dsl::newline);
    static constexpr auto value = lexy::callback<helpers::CanFrame>(
        [](helpers::CanFrame&& f) { return std::move(f); },
        [](helpers::CanFrame&& f, lexy::nullopt) { return std::move(f); },
        [](helpers::CanFrame&& f, lexy::nullopt, lexy::nullopt) { return std::move(f); }
    );
};

// Entire TRC file → vector of frames only
struct trc_file
{
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = dsl::terminator(dsl::eof).opt_list(
        dsl::while_( (dsl::peek(LEXY_LIT(";")) >> dsl::p<comment_line>)
                   | (dsl::peek(dsl::newline)   >> dsl::p<blank_line>) )
        + dsl::p<frame_line>
    );
    static constexpr auto value = lexy::as_list<std::vector<helpers::CanFrame>>;
};

}  // namespace

std::size_t parse_file(const std::string& path,
                       const std::function<bool(const helpers::CanFrame&)>& on_frame)
{
    if (on_frame == nullptr)
    {
        return 0;
    }

    std::ifstream in(path);
    if (!in.is_open())
    {
        return 0;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string contents = ss.str();
    return parse_string(contents.c_str(), on_frame);
}

std::size_t parse_string(const char* trc_contents,
                         const std::function<bool(const helpers::CanFrame&)>& on_frame)
{
    if (trc_contents == nullptr)
    {
        return 0;
    }

    if (on_frame == nullptr)
    {
        return 0;
    }

    const std::string_view sv{trc_contents};
    auto input = lexy::string_input<lexy::utf8_encoding>(sv.data(), sv.size());

    auto result = lexy::parse<trc_file>(input, lexy_ext::report_error);
    if (!result.has_value())
    {
        return 0;
    }
    const auto& lines = result.value();
    std::size_t delivered = 0;
    for (const auto& frame : lines)
    {
        if (!on_frame(frame)) break;
        ++delivered;
    }
    return delivered;
}

}  // namespace pcan_trc_parser


