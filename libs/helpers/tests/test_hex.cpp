// SPDX-License-Identifier: GPL-3.0-or-later
//
// helpers::toHex / helpers::fromHex.
//
// The rejections are the point. The copies this replaced skipped a bad digit or
// dropped an odd trailing nibble, so a typo in a test fixture decoded to
// different bytes than the literal said -- and the test then passed against
// bytes nobody wrote down.
//
// Mutation-check: make fromHex skip characters it does not recognise, and the
// "not a hex digit" case fails.

#include "helpers/hex.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using Bytes = std::vector<std::uint8_t>;

bool rejected(std::string_view text, std::string_view needle)
{
    std::string error;
    const auto bytes = helpers::fromHex(text, &error);
    return !bytes && error.find(needle) != std::string::npos;
}

}  // namespace

int main()
{
    const Bytes sample{0x00, 0x01, 0x7a, 0xab, 0xff};

    expect(helpers::toHex(sample) == "00017aabff", "lowercase, no separator by default");
    expect(helpers::toHex(sample, " ", helpers::HexCase::kUpper) == "00 01 7A AB FF",
           "separator and uppercase on request");
    expect(helpers::toHex(Bytes{}).empty(), "no bytes, no text");
    expect(helpers::toHex(Bytes{0x42}, ":") == "42", "a single byte has no separator");

    expect(helpers::fromHex("00017aabff") == sample, "plain hex parses");
    expect(helpers::fromHex("00 01:7A\nAB\tff") == sample, "separators between bytes and either case");
    expect(helpers::fromHex("0x00017aabff") == sample, "a leading 0x is accepted");
    expect(helpers::fromHex("") == Bytes{}, "empty text is no bytes");
    expect(helpers::fromHex(helpers::toHex(sample, " ")) == sample, "toHex output round-trips");

    expect(rejected("abc", "odd number"), "an odd digit count is refused, not truncated");
    expect(rejected("0g", "not a hex digit"), "a non-hex character is refused, not skipped");
    expect(rejected("0 1", "splits a byte"), "a separator inside a byte is refused");
    expect(!helpers::fromHex("zz").has_value(), "the error out-parameter is optional");

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
