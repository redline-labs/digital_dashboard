#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dbc_parser
{

struct ValueMapping
{
    int64_t rawValue{};
    std::string description{};
};

struct Signal
{
    std::string name{};
    uint32_t startBit{};
    uint32_t length{};
    bool littleEndian{true}; // @1 means big endian in DBC; using true for little-endian (@1 == Motorola/big)
    bool isSigned{false};
    // Multiplexing
    bool isMultiplex{false};      // true if this signal is gated by a multiplexor (has form m<idx>)
    bool isMultiplexor{false};    // true if this signal is the multiplexor (has form M)
    uint32_t multiplexedGroupIdx{}; // numeric group index for multiplexed signals (m<idx>)
    double scale{1.0};
    double offset{0.0};
    double minimum{0.0};
    double maximum{0.0};
    std::string unit{};
    std::vector<std::string> receivers{};
    std::vector<ValueMapping> valueTable{}; // from VAL_
    std::string comment{}; // from CM_ SG_
};

struct Message
{
    uint32_t id{};
    std::string name{};
    uint32_t dlc{};
    std::string transmitter{};
    std::string comment{};
    bool isMultiplexed{false}; // true if any signal is multiplexor or multiplexed
    std::vector<Signal> signals{};
};

struct Database
{
    std::string version{}; // VERSION "..."
    std::vector<std::string> nodes{}; // BU_ : A B C
    std::vector<Message> messages{};  // BO_
};

} // namespace dbc_parser


