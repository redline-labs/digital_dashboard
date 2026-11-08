#pragma once

// Decoding at run time, straight from a parsed Database.
//
// The generated decoders are what a node uses: typed, checked at compile time,
// one per DBC chosen at build time. This is for a program that does not know
// its DBC until it runs -- a simulator reporting the signals on its bus, a tool
// pointed at a file -- and it walks the same bits: tests/test_runtime_decode
// holds it to the same cantools goldens the generated code is held to.

#include "dbc_parser/ast.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace dbc_parser
{

// The signal's raw field, zero-extended, or nothing when the payload is too
// short to hold it. A short frame is not decoded as if padded: a missing byte
// read as zero is a plausible wrong number.
std::optional<uint64_t> rawBits(const Signal &signal, std::span<const uint8_t> payload);

// The physical value: the raw field sign-extended or reinterpreted as IEEE as
// the signal says, then scaled and offset. Nothing when the payload is short.
std::optional<double> decodeSignal(const Signal &signal, std::span<const uint8_t> payload);

// Every signal the payload carries, by name. A multiplexed signal only when the
// multiplexor's value selects its group; one whose multiplexor cannot be read
// is left out with it.
std::map<std::string, double> decodeMessage(const Message &message, std::span<const uint8_t> payload);

const Message *findMessage(const Database &database, uint32_t id, bool isExtended);
const Message *findMessageWithSignal(const Database &database, std::string_view signal);

} // namespace dbc_parser
