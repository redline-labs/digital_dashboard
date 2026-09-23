// Parser behaviour: error reporting, run-time parsing, and load_cof on small inputs.
#include <cstdio>
#include <string_view>

#include "wmm/wmmhr2025.h"

#include "embedded_model.h"

namespace {

using namespace std::string_view_literals;

constexpr auto tiny = R"(    2020.0            WMM-TINY      12/10/2019
    1  0  -29404.5       0.0        6.7        0.0
    1  1   -1450.7    4652.9        7.7      -25.1
999999999999999999999999999999999999999999999999
999999999999999999999999999999999999999999999999
)"sv;

constexpr auto r = wmm::parse_cof<1>(tiny);
static_assert(!r.error);
static_assert(r.model.name() == "WMM-TINY");
static_assert(r.model.epoch == 2020.0);
static_assert(r.model.release_date == wmm::Date{2019, 12, 10});
static_assert(r.model(1, 1).h == 4652.9 && r.model(1, 1).h_dot == -25.1);

// CRLF line endings, blank lines, no terminator, exponent notation.
static_assert(!wmm::parse_cof<1>("2020.0 X 01/01/2020\r\n\r\n1 0 -2.94045e4 0 6.7 0\r\n1 1 -1450.7 4652.9 7.7 -25.1"sv).error);
static_assert(wmm::parse_cof<1>("2020.0 X 01/01/2020\n1 0 -2.94045e4 0 6.7 0\n1 1 1 1 1 1\n"sv).model(1, 0).g == -29404.5);

constexpr wmm::ParseError error_of(std::string_view text) { return wmm::parse_cof<1>(text).error; }

static_assert(error_of("2020.0 X 13/01/2020\n"sv).what == "expected release date as mm/dd/yyyy");
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 1 1\n1 1 1 1 x 1\n"sv).line == 3);
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 1 1\n1 1 1 1 1 1 1\n"sv).what == "unexpected text after coefficients");
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 1 1\n1 2 1 1 1 1\n"sv).what == "degree/order out of range");
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 1 1\n1 0 1 1 1 1\n"sv).what == "duplicate (n, m) entry");
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 1 1\n"sv).what == "missing (n, m) entries below the maximum degree");
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 1 1\n1 1 1 1 1 1\n2 0 1 1 1 1\n"sv).what ==
              "degree exceeds the model's maximum degree");

// Malformed input a damaged or hand-edited file could hold. Each must stop the
// parse at the right line rather than yield a plausible coefficient.
static_assert(error_of(""sv).what == "expected model epoch (decimal year)");
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1"sv).line == 2);                  // truncated record
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 nan 1\n1 1 1 1 1 1\n"sv).line == 2);  // NaN coefficient
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1 1 inf 1\n1 1 1 1 1 1\n"sv).line == 2);  // infinite coefficient
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1e999 1 1 1\n1 1 1 1 1 1\n"sv).line == 2);  // overflows to inf
static_assert(error_of("2020.0 X 01/01/2020\n1 0 9e308 1 1 1\n1 1 1 1 1 1\n"sv).line == 2);  // rounds up to inf
static_assert(error_of("2020.0 X 01/01/2020\n1 0 1e999999999 1 1 1\n1 1 1 1 1 1\n"sv).line == 2);  // would spin the evaluator
static_assert(error_of("2020.0 X 01/01/2020\n-1 0 1 1 1 1\n"sv).line == 2);            // negative degree
static_assert(error_of("2020.0 X 01/01/2020\n1 -1 1 1 1 1\n"sv).line == 2);            // negative order
static_assert(error_of("2020.0 X 02/30/2020\n"sv).what == "expected release date as mm/dd/yyyy");
static_assert(error_of("nan X 01/01/2020\n1 0 1 1 1 1\n1 1 1 1 1 1\n"sv).line == 1);  // NaN epoch
static_assert(error_of("2020.0 X 01/01/2020\n"sv).what == "missing (n, m) entries below the maximum degree");

// load_cof on a string literal (the trailing NUL is ignored) picks the degree up
// from the file.
constexpr char tiny_text[] = "2020.0 X 01/01/2020\n1 0 1 2 3 4\n1 1 5 6 7 8\n2 0 0 0 0 0\n2 1 0 0 0 0\n2 2 0 0 0 9\n";
constexpr auto tiny_model = wmm::load_cof<tiny_text>();
static_assert(tiny_model.nmax == 2 && tiny_model(2, 2).h_dot == 9);

// Decimal conversion is correctly rounded (matches strtod).
static_assert(wmm::parse_cof<1>("0 X 01/01/2020\n1 0 0.1 0.3 -1234.5678 1e-5\n1 1 0 0 0 0\n"sv).model(1, 0) ==
              wmm::Coefficient{0.1, 0.3, -1234.5678, 1e-5});

}  // namespace

int main() {
    // The same parser at run time over the full WMMHR file, against the model the
    // compiled library parsed at compile time.
    const std::string_view text(test::wmmhr2025_cof, sizeof test::wmmhr2025_cof);
    const auto runtime = wmm::parse_cof<133>(text);
    const auto& library = wmm::wmmhr2025::model();
    const bool same = !runtime.error && runtime.model.coeffs == library.coeffs &&
                      runtime.model.name() == library.name() && runtime.model.epoch == library.epoch &&
                      runtime.model.release_date == library.release_date;
    std::printf("run-time parse of WMMHR.COF %s wmm::wmmhr2025::model()\n", same ? "matches" : "DIFFERS from");
    return same ? 0 : 1;
}
