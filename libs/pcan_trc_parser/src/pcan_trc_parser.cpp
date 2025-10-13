#include "pcan_trc_parser/pcan_trc_parser.h"

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

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool parse_hex_uint(std::string_view s, std::uint32_t& value_out)
{
    std::uint32_t value = 0;
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_hex_digit(c)) return false;
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(10 + (c - 'a'));
        else value |= static_cast<std::uint32_t>(10 + (c - 'A'));
    }
    value_out = value;
    return true;
}

static inline void trim_left(std::string_view& s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
}

static inline std::string_view take_token(std::string_view& s)
{
    trim_left(s);
    std::size_t i = 0;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::string_view token = s.substr(0, i);
    s.remove_prefix(i);
    return token;
}

static bool parse_line(std::string_view line, PcanTrcFrame& out_frame)
{
    // Ignore comments/headers that start with ';' or empty lines
    std::string_view sv = line;
    trim_left(sv);
    if (sv.empty() || sv.front() == ';') return false;

    // Expected column mapping per sample:
    // Number, TimeOffset(ms), Type("DT"), ID(hex), Direction(Rx/Tx), DLC, 8 bytes (hex)
    // Example:
    // "      1 4294967269.343 DT     0500 Rx 8  40 00 00 00 00 00 00 00"

    PcanTrcFrame frame{};

    // Message number
    {
        std::string_view tok = take_token(sv);
        if (tok.empty()) return false;
        std::uint64_t num = 0;
        for (char c : tok) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            num = num * 10 + static_cast<unsigned>(c - '0');
        }
        frame.messageNumber = num;
    }

    // Timestamp (ms, can be very large / fractional)
    {
        std::string_view tok = take_token(sv);
        if (tok.empty()) return false;
        std::string tmp(tok);
        char* end = nullptr;
        frame.timestampMs = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
    }

    // Type (often "DT"), skip token but validate minimal length
    {
        std::string_view tok = take_token(sv);
        if (tok.empty()) return false;  // require presence, but we don't store it
    }

    // ID (hex)
    {
        std::string_view tok = take_token(sv);
        if (tok.empty()) return false;
        std::uint32_t id_temp = 0;
        if (!parse_hex_uint(tok, id_temp)) return false;
        frame.id = id_temp;
    }

    // Direction (Rx/Tx)
    {
        std::string_view tok = take_token(sv);
        if (tok == "Rx") frame.direction = direction_t::Rx;
        else if (tok == "Tx") frame.direction = direction_t::Tx;
        else return false;
    }

    // DLC
    {
        std::string_view tok = take_token(sv);
        if (tok.empty()) return false;
        std::uint32_t dlc = 0;
        for (char c : tok) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            dlc = dlc * 10 + static_cast<unsigned>(c - '0');
            if (dlc > 64) return false;  // sanity
        }
        frame.dlc = static_cast<std::uint8_t>(dlc);
        frame.payload.reserve(frame.dlc);
    }

    // Payload bytes (hex, separated by spaces) - may be fewer than DLC in some files
    for (std::size_t i = 0; i < frame.dlc; ++i) {
        trim_left(sv);
        if (sv.empty()) break;
        std::string_view tok = take_token(sv);
        if (tok.empty()) break;
        std::uint32_t byte_val = 0;
        if (!parse_hex_uint(tok, byte_val) || byte_val > 0xFFu) return false;
        frame.payload.push_back(static_cast<std::uint8_t>(byte_val));
    }

    out_frame = std::move(frame);
    return true;
}

}  // namespace

std::size_t parse_file(const std::string& path,
                       const std::function<bool(const PcanTrcFrame&)>& on_frame)
{
    std::ifstream in(path);
    if (!in.is_open()) return 0;
    std::size_t delivered = 0;
    std::string line;
    while (std::getline(in, line)) {
        PcanTrcFrame f{};
        if (!parse_line(line, f)) continue;
        if (!on_frame(f)) break;
        ++delivered;
    }
    return delivered;
}

std::size_t parse_string(const char* trc_contents,
                         const std::function<bool(const PcanTrcFrame&)>& on_frame)
{
    if (trc_contents == nullptr) return 0;
    std::istringstream in{std::string(trc_contents)};
    std::size_t delivered = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        PcanTrcFrame f{};
        if (!parse_line(std::string_view(line), f)) continue;
        if (!on_frame(f)) break;
        ++delivered;
    }
    return delivered;
}

}  // namespace pcan_trc_parser


