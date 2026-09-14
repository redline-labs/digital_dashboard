#ifndef HELPERS_HEX_H_
#define HELPERS_HEX_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace helpers
{

// Bytes <-> hex text. One implementation, because the tree had grown six: two
// in the plist tests, one each in the airplay, MOTEC and DBC tests, and one in
// pub_sub -- each with its own idea of case, separators and what a bad digit
// means. The copies that silently skipped a bad digit or an odd trailing nibble
// could make a test fixture decode to different bytes than the literal says,
// and the test would still pass against those.

enum class HexCase
{
    kLower,
    kUpper,
};

// Two digits per byte, `separator` between bytes (none by default).
//
//   toHex({0x01, 0xab})                   -> "01ab"
//   toHex({0x01, 0xab}, " ", kUpper)      -> "01 AB"
inline std::string toHex(std::span<const std::uint8_t> bytes, std::string_view separator = {},
                         HexCase letter_case = HexCase::kLower)
{
    const char* const digits =
        letter_case == HexCase::kUpper ? "0123456789ABCDEF" : "0123456789abcdef";

    std::string out;
    out.reserve(bytes.size() * (2 + separator.size()));
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i != 0)
        {
            out += separator;
        }
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0f]);
    }
    return out;
}

// Parses hex text. Either case; an optional leading 0x; whitespace and ':'
// allowed BETWEEN bytes ("01 ff:7a", or a literal split across lines).
//
// nullopt, with `*error` saying why when given, for:
//   * a character that is not a hex digit or an allowed separator,
//   * an odd number of digits -- every byte needs two,
//   * a separator inside a byte ("0 1"), which would make the grouping a guess.
// Never skips or pads, so the bytes are exactly the ones written.
inline std::optional<std::vector<std::uint8_t>> fromHex(std::string_view text,
                                                        std::string* error = nullptr)
{
    const auto fail = [error](std::string why) -> std::optional<std::vector<std::uint8_t>>
    {
        if (error != nullptr)
        {
            *error = std::move(why);
        }
        return std::nullopt;
    };

    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
    }

    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);

    int high = -1;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':')
        {
            if (high >= 0)
            {
                return fail("a separator splits a byte at position " + std::to_string(i) + ".");
            }
            continue;
        }

        int nibble = -1;
        if (c >= '0' && c <= '9')
        {
            nibble = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            nibble = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            nibble = c - 'A' + 10;
        }
        else
        {
            return fail(std::string("'") + c + "' at position " + std::to_string(i) +
                        " is not a hex digit.");
        }

        if (high < 0)
        {
            high = nibble;
        }
        else
        {
            out.push_back(static_cast<std::uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }

    if (high >= 0)
    {
        return fail("odd number of hex digits; every byte needs two.");
    }
    return out;
}

}  // namespace helpers

#endif  // HELPERS_HEX_H_
