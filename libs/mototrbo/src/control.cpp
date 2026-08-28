// SPDX-License-Identifier: GPL-3.0-or-later

#include "mototrbo/control.h"

namespace mototrbo::control
{

namespace
{

void append_utf8(std::string& out, char32_t codepoint)
{
    if (codepoint < 0x80)
    {
        out.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint < 0x800)
    {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
    else
    {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

// One UTF-16BE code unit at `index`, which the caller has bounds-checked.
char32_t unit_at(std::span<const std::uint8_t> bytes, std::size_t index)
{
    return static_cast<char32_t>(read_u16(bytes, index));
}

// The separator the radio puts between softkey labels on the bottom line.
constexpr char32_t kSoftkeySeparator = 0xEFCD;

constexpr char32_t kEscape = 0x1B;

} // namespace

std::string decode_display_text(std::span<const std::uint8_t> utf16be)
{
    std::string out;

    for (std::size_t i = 0; i + 1 < utf16be.size(); i += 2)
    {
        const char32_t unit = unit_at(utf16be, i);

        if (unit == kEscape)
        {
            // ANSI CSI: ESC '[' parameters, then a final byte in @..~ . The
            // radio uses them for cursor positioning and colour, and they are
            // in the middle of the text rather than around it -- a decode that
            // does not strip them renders the escapes as glyphs.
            std::size_t j = i + 2;
            if (j + 1 < utf16be.size() && unit_at(utf16be, j) == U'[')
            {
                j += 2;
                while (j + 1 < utf16be.size())
                {
                    const char32_t parameter = unit_at(utf16be, j);
                    j += 2;
                    if (parameter >= U'@' && parameter <= U'~')
                    {
                        break;
                    }
                }
                // The loop's own increment steps past the last unit consumed.
                i = j - 2;
            }
            continue;
        }

        if (unit == kSoftkeySeparator)
        {
            out += " | ";
            continue;
        }

        // NUL padding and the other control codes.
        if (unit < 0x20)
        {
            continue;
        }

        append_utf8(out, unit);
    }

    const std::size_t first = out.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return {};
    }

    return out.substr(first, out.find_last_not_of(" \t") - first + 1);
}

} // namespace mototrbo::control
