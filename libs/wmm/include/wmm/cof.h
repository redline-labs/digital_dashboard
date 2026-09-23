// Compile-time parser for NOAA/NCEI ".COF" spherical-harmonic coefficient files
// (WMM, WMMHR, and anything else in the same format).
//
// File layout:
//
//     <epoch> <model-name> <release-date mm/dd/yyyy>
//     <n> <m> <g> <h> <g-dot> <h-dot>
//     ...
//     9999...                                   (terminator line, optional)
//
// Everything here is constexpr, so a coefficient file embedded with #embed can be
// turned into a fully-populated `Model<N>` during constant evaluation; see
// `load_cof` at the bottom, and src/wmmhr2025.cpp for a concrete model.
#pragma once
#define WMM_COF_H_INCLUDED

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>

#include "wmm/model.h"
#include "wmm/types.h"

namespace wmm {

// ---------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------

struct ParseError {
    std::string_view what;  // empty means "no error"
    std::size_t line = 0;   // 1-based; 0 when the error is not tied to a line

    constexpr explicit operator bool() const { return !what.empty(); }
};

// A parse error, carried as a template argument so it survives into the
// compiler's diagnostic. C++26 lets static_assert print a computed message;
// C++23 does not, so load_cof() instantiates CofParseError<Where{...}> and
// the error names the file line in the instantiation trace -- GCC prints the
// message text too, clang prints it as character codes beside the line.
struct CofErrorSite {
    char what[64]{};
    std::size_t line = 0;

    constexpr CofErrorSite(ParseError e) : line(e.line) {
        for (std::size_t i = 0; i < e.what.size() && i + 1 < sizeof what; ++i) what[i] = e.what[i];
    }
};

template <CofErrorSite Where>
struct CofParseError {
    static_assert(sizeof(Where) == 0, "COF parse error: the template argument above holds the message and line");
};

namespace detail {

// The parser below runs inside clang's constant evaluator, which interprets the
// AST directly. For a ~0.5 MB file the cost is dominated by per-character work,
// so the hot loops deliberately use raw pointers and inline character tests
// rather than string_view members or small helper calls.

constexpr bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr void skip_blanks(const char*& p, const char* end) {
    while (p != end && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
}

// True if the field just read ends at a separator.
constexpr bool at_separator(const char* p, const char* end) {
    return p == end || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
}

// [+-]digits
constexpr bool read_int(const char*& p, const char* end, int& out) {
    skip_blanks(p, end);
    bool negative = false;
    if (p != end && (*p == '+' || *p == '-')) negative = *p++ == '-';
    int v = 0;
    int digits = 0;
    for (; p != end && *p >= '0' && *p <= '9'; ++p) {
        if (++digits > 9) return false;
        v = v * 10 + (*p - '0');
    }
    out = negative ? -v : v;
    return digits != 0 && at_separator(p, end);
}

// Decimal floating point: [+-]digits[.digits][(e|E)[+-]digits].
//
// The digits are accumulated into an integer mantissa and scaled by a single
// multiply or divide by an exact power of ten. When the mantissa fits in 53 bits
// and the power is <= 1e22 (always the case for COF files) both operands are
// exact, so the one IEEE operation yields the correctly rounded value -- bit
// identical to what strtod/sscanf produce at run time.
constexpr bool read_double(const char*& p, const char* end, double& out) {
    constexpr double pow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    skip_blanks(p, end);
    bool negative = false;
    if (p != end && (*p == '+' || *p == '-')) negative = *p++ == '-';

    // Single-statement loop bodies: each statement is a constant-evaluator step.
    std::uint64_t mantissa = 0;
    const char* const int_begin = p;
    for (; p != end && *p >= '0' && *p <= '9'; ++p) mantissa = mantissa * 10 + static_cast<unsigned>(*p - '0');
    int digits = static_cast<int>(p - int_begin);
    int exp10 = 0;
    if (p != end && *p == '.') {
        const char* const frac_begin = ++p;
        for (; p != end && *p >= '0' && *p <= '9'; ++p) mantissa = mantissa * 10 + static_cast<unsigned>(*p - '0');
        exp10 = -static_cast<int>(p - frac_begin);
        digits -= exp10;
    }
    if (digits == 0 || digits > 19) return false;  // > 19 digits would overflow the mantissa
    if (p != end && (*p == 'e' || *p == 'E')) {
        int e = 0;
        ++p;
        if (!read_int(p, end, e)) return false;
        exp10 += e;
    }
    if (!at_separator(p, end)) return false;
    // Past a double's range the scaling below overflows to inf -- a coefficient
    // that parses and poisons every field value after it -- and an exponent in
    // the billions would spin the constant evaluator out of steps first.
    if (exp10 > 308 || exp10 < -400) return false;

    double v = static_cast<double>(mantissa);
    // Exact fast path above; very large exponents fall back to repeated scaling,
    // which is not guaranteed correctly rounded (never needed for COF files).
    // Refuse before multiplying, not after: GCC does not treat a floating
    // overflow as a constant expression, so the product must never be inf.
    constexpr double max_finite = 1.7976931348623157e308;
    for (; exp10 > 22; exp10 -= 22) {
        if (v > max_finite / 1e22) return false;
        v *= 1e22;
    }
    for (; exp10 < -22; exp10 += 22) v /= 1e22;
    if (exp10 >= 0 && v > max_finite / pow10[exp10]) return false;
    v = exp10 >= 0 ? v * pow10[exp10] : v / pow10[-exp10];
    out = negative ? -v : v;
    return true;
}

// Next whitespace-delimited token on the current line.
constexpr std::string_view read_token(const char*& p, const char* end) {
    skip_blanks(p, end);
    const char* begin = p;
    while (p != end && !is_blank(*p) && *p != '\n') ++p;
    return {begin, static_cast<std::size_t>(p - begin)};
}

constexpr bool parse_uint(std::string_view s, int& out) {
    if (s.empty() || s.size() > 9) return false;
    out = 0;
    for (char c : s) {
        if (!is_digit(c)) return false;
        out = out * 10 + (c - '0');
    }
    return true;
}

// "mm/dd/yyyy"
constexpr bool parse_date(std::string_view s, Date& out) {
    auto slash1 = s.find('/');
    auto slash2 = slash1 == s.npos ? s.npos : s.find('/', slash1 + 1);
    if (slash2 == s.npos) return false;
    Date d;
    if (!parse_uint(s.substr(0, slash1), d.month) ||
        !parse_uint(s.substr(slash1 + 1, slash2 - slash1 - 1), d.day) ||
        !parse_uint(s.substr(slash2 + 1), d.year) || !is_valid(d))
        return false;
    out = d;
    return true;
}

// True if [p, end) is blank up to the end of the line; consumes the '\n'.
constexpr bool end_line(const char*& p, const char* end) {
    skip_blanks(p, end);
    if (p == end) return true;
    if (*p != '\n') return false;
    ++p;
    return true;
}

// Terminator line (as in MAG_readMagneticModel: starts with "9999").
constexpr bool is_terminator(const char* p, const char* end) {
    return end - p >= 4 && p[0] == '9' && p[1] == '9' && p[2] == '9' && p[3] == '9';
}

}  // namespace detail

// One coefficient record as it appears in the file.
struct CofRecord {
    int n = 0;
    int m = 0;
    Coefficient c;
};

// Parse state after a run of lines. A file is consumed as a sequence of chunks so
// that no single constant evaluation has to walk the whole text; see load_cof.
template <std::size_t Capacity>
struct CofChunk {
    // Header fields (only filled by the chunk that starts at offset 0).
    double epoch = 0.0;
    std::array<char, 32> name_buf{};
    std::size_t name_len = 0;
    Date release_date{};

    std::array<CofRecord, Capacity> records{};
    std::size_t count = 0;
    int max_degree = 0;

    std::size_t end_offset = 0;  // where the next chunk starts
    std::size_t end_line = 1;    // line number at end_offset
    bool done = false;           // reached the terminator or end of text
    ParseError error{};
};

// Parses up to Capacity records starting at `offset` (line number `line`). The
// chunk at offset 0 also reads the header line.
template <std::size_t Capacity>
constexpr CofChunk<Capacity> parse_cof_chunk(std::string_view text, std::size_t offset = 0,
                                             std::size_t line = 1) {
    using namespace detail;
    CofChunk<Capacity> out;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const char* p = begin + offset;
    auto fail = [&](std::string_view what) {
        out.error = {what, line};
        out.done = true;
        return out;
    };

    if (offset == 0) {  // <epoch> <name> <mm/dd/yyyy>
        if (!read_double(p, end, out.epoch)) return fail("expected model epoch (decimal year)");
        auto name = read_token(p, end);
        if (name.empty()) return fail("expected model name");
        if (name.size() >= out.name_buf.size()) return fail("model name is too long");
        for (std::size_t i = 0; i < name.size(); ++i) out.name_buf[i] = name[i];
        out.name_len = name.size();
        if (!parse_date(read_token(p, end), out.release_date))
            return fail("expected release date as mm/dd/yyyy");
        if (!end_line(p, end)) return fail("unexpected text after header");
        ++line;
    }

    for (; p != end && out.count != Capacity; ++line) {
        skip_blanks(p, end);
        if (p != end && *p == '\n') {  // blank line
            ++p;
            continue;
        }
        if (is_terminator(p, end)) {
            p = end;
            break;
        }
        CofRecord& r = out.records[out.count];
        if (!read_int(p, end, r.n) || !read_int(p, end, r.m))
            return fail("expected integer degree and order");
        if (r.n < 1 || r.m < 0 || r.m > r.n) return fail("degree/order out of range");
        if (!read_double(p, end, r.c.g) || !read_double(p, end, r.c.h) ||
            !read_double(p, end, r.c.g_dot) || !read_double(p, end, r.c.h_dot))
            return fail("expected four coefficients: g h g_dot h_dot");
        if (!end_line(p, end)) return fail("unexpected text after coefficients");
        if (r.n > out.max_degree) out.max_degree = r.n;
        ++out.count;
    }
    out.end_offset = static_cast<std::size_t>(p - begin);
    out.end_line = line;
    out.done = p == end;
    return out;
}

template <int NMax>
struct CofResult {
    Model<NMax> model{};
    ParseError error{};
};

namespace detail {

// Accumulates chunks into a model, checking for duplicate and missing terms.
template <int NMax>
struct CofAssembler {
    CofResult<NMax> result{};
    std::array<bool, num_terms(NMax)> seen{};
    std::size_t count = 0;

    template <std::size_t Capacity>
    constexpr bool add(const CofChunk<Capacity>& chunk, bool has_header) {
        if (chunk.error) {
            result.error = chunk.error;
            return false;
        }
        if (has_header) {
            result.model.epoch = chunk.epoch;
            result.model.name_buf = chunk.name_buf;
            result.model.name_len = chunk.name_len;
            result.model.release_date = chunk.release_date;
        }
        for (std::size_t i = 0; i < chunk.count; ++i) {
            const CofRecord& r = chunk.records[i];
            if (r.n > NMax) {
                result.error = {"degree exceeds the model's maximum degree"};
                return false;
            }
            const auto k = term_index(r.n, r.m);
            if (seen[k]) {
                result.error = {"duplicate (n, m) entry"};
                return false;
            }
            seen[k] = true;
            result.model.coeffs[k] = r.c;
        }
        count += chunk.count;
        return true;
    }

    constexpr CofResult<NMax> finish() {
        if (!result.error && count != num_terms(NMax) - 1)
            result.error = {"missing (n, m) entries below the maximum degree"};
        return result;
    }
};

}  // namespace detail

// Parses a whole coefficient file into a Model<NMax> in one call. Works at run
// time (e.g. on a file read from disk) and in constant evaluation for small
// files; for large embedded files prefer load_cof, which stays inside the
// compiler's default constexpr step budget.
template <int NMax>
constexpr CofResult<NMax> parse_cof(std::string_view text) {
    detail::CofAssembler<NMax> a;
    std::size_t offset = 0, line = 1;
    for (bool done = false; !done;) {
        auto chunk = parse_cof_chunk<256>(text, offset, line);
        if (!a.add(chunk, offset == 0)) break;
        offset = chunk.end_offset;
        line = chunk.end_line;
        done = chunk.done;
    }
    return a.finish();
}

namespace detail {

// Lines per compile-time chunk. clang's default budget (-fconstexpr-steps) is
// 2^20 steps per constant evaluation and parsing costs roughly 4 steps per
// character, so ~1000 lines of ~60 characters stays comfortably below it.
inline constexpr std::size_t load_chunk_lines = 1024;

// The embedded text; a trailing NUL (string literal rather than #embed) is dropped.
template <const auto& Text>
inline constexpr std::string_view embedded_text{
    std::data(Text), std::size(Text) - (std::size(Text) != 0 && std::data(Text)[std::size(Text) - 1] == '\0')};

// Chunk K of Text. Each is a separate constexpr variable and therefore a
// separate constant evaluation with its own step budget; chunk K starts where
// chunk K - 1 stopped.
template <const auto& Text, std::size_t K>
struct EmbeddedChunk {
    static constexpr auto value =
        parse_cof_chunk<load_chunk_lines>(embedded_text<Text>, EmbeddedChunk<Text, K - 1>::value.end_offset,
                                          EmbeddedChunk<Text, K - 1>::value.end_line);
};

template <const auto& Text>
struct EmbeddedChunk<Text, 0> {
    static constexpr auto value = parse_cof_chunk<load_chunk_lines>(embedded_text<Text>);
};

template <const auto& Text, std::size_t K>
inline constexpr const auto& cof_chunk = EmbeddedChunk<Text, K>::value;

template <const auto& Text, std::size_t K = 0>
consteval std::size_t chunk_count() {
    if constexpr (cof_chunk<Text, K>.done)
        return K + 1;
    else
        return chunk_count<Text, K + 1>();
}

template <const auto& Text, std::size_t... K>
consteval int max_degree(std::index_sequence<K...>) {
    return std::max({cof_chunk<Text, K>.max_degree...});
}

template <const auto& Text, int NMax, std::size_t... K>
consteval CofResult<NMax> assemble(std::index_sequence<K...>) {
    CofAssembler<NMax> a;
    (a.add(cof_chunk<Text, K>, K == 0) && ...);
    return a.finish();
}

}  // namespace detail

// Builds a Model from a coefficient file available as a constant expression,
// e.g. an array filled by #embed. Problems in the file become compile errors
// naming the offending line.
//
//     static constexpr char text[] = {
//     #embed "WMM.COF"
//     };
//     constexpr auto wmm = wmm::load_cof<text>();
//
// The model degree is taken from the file, and the text is parsed in chunks
// (one constant evaluation each) so no -fconstexpr-steps adjustment is needed.
template <const auto& Text>
consteval auto load_cof() {
    using Seq = std::make_index_sequence<detail::chunk_count<Text>()>;
    constexpr int nmax = detail::max_degree<Text>(Seq{});
    constexpr auto result = [] {
        if constexpr (nmax >= 1)
            return detail::assemble<Text, nmax>(Seq{});
        else
            return CofResult<1>{.error = detail::cof_chunk<Text, 0>.error
                                             ? detail::cof_chunk<Text, 0>.error
                                             : ParseError{"no coefficient records found"}};
    }();
    if constexpr (result.error) static_cast<void>(sizeof(CofParseError<CofErrorSite{result.error}>));
    return result.model;
}

}  // namespace wmm
