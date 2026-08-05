#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dbc_parser
{

struct ValueMapping
{
    int64_t rawValue{};
    std::string description{};
};

// How the raw bits are to be read. Integer is the DBC default; the other two
// come from SIG_VALTYPE_, and getting them wrong is silent -- an IEEE float
// read as an integer is a plausible-looking wrong number, not an error.
enum class SignalValueType
{
    Integer,
    Float,  // 32-bit IEEE 754
    Double, // 64-bit IEEE 754
};

struct Signal
{
    std::string name{};
    uint32_t startBit{};
    uint32_t length{};
    // DBC spells this `@1` for Intel/little-endian and `@0` for Motorola/big.
    bool littleEndian{true};
    bool isSigned{false};
    SignalValueType valueType{SignalValueType::Integer};

    // Multiplexing. A signal may be both: `m3M` gates on group 3 and selects
    // the groups below it.
    bool isMultiplex{false};   // gated by a multiplexor (form m<idx>)
    bool isMultiplexor{false}; // selects the group (form M)
    uint32_t multiplexedGroupIdx{};

    double scale{1.0};
    double offset{0.0};
    double minimum{0.0};
    double maximum{0.0};
    std::string unit{};
    std::vector<std::string> receivers{};
    std::vector<ValueMapping> valueTable{}; // from VAL_, or a named VAL_TABLE_
    std::string comment{};                  // from CM_ SG_

    // Highest bit index the signal occupies, in the flat 0..8*dlc-1 space the
    // decoder walks. Used to prove the signal fits inside the frame.
    uint32_t lastBitIndex() const;
};

struct Message
{
    // The 11- or 29-bit identifier, with the DBC extended-frame flag already
    // stripped. Comparing a raw DBC id against an id from a CAN driver matches
    // nothing, because only the file sets bit 31.
    uint32_t id{};
    bool isExtended{false};

    std::string name{};
    uint32_t dlc{};
    std::string transmitter{};
    std::string comment{};
    bool isMultiplexed{false}; // any signal is multiplexor or multiplexed
    std::vector<Signal> signals{};

    // Attribute values from BA_, by attribute name.
    std::map<std::string, std::string> attributes{};

    const Signal *multiplexor() const;
};

struct Database
{
    std::string version{};            // VERSION "..."
    std::vector<std::string> nodes{}; // BU_ : A B C
    std::vector<Message> messages{};  // BO_

    // VAL_TABLE_ definitions, by table name. A VAL_ line may name one of these
    // instead of listing pairs inline.
    std::map<std::string, std::vector<ValueMapping>> valueTables{};
};

} // namespace dbc_parser
