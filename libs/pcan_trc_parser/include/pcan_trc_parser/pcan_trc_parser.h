#ifndef PCAN_TRC_PARSER_H_
#define PCAN_TRC_PARSER_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pcan_trc_parser
{

enum class direction_t
{
    Rx,
    Tx
};

struct PcanTrcFrame {
    std::uint64_t messageNumber;
    double timestampMs;
    std::uint32_t id;
    direction_t direction;
    std::uint8_t dlc;
    std::vector<std::uint8_t> payload;
};

// Parse a PCAN .trc file and invoke the callback for each parsed frame.
// The callback should return true to continue parsing, or false to stop early.
// Returns the number of frames successfully delivered to the callback.
std::size_t parse_file(const std::string& path,
                       const std::function<bool(const PcanTrcFrame&)>& on_frame);

// Parse PCAN .trc contents provided as a NUL-terminated C-string.
// Uses the same callback interface as parse_file. Returns delivered frames.
std::size_t parse_string(const char* trc_contents,
                         const std::function<bool(const PcanTrcFrame&)>& on_frame);

}  // namespace pcan_trc_parser

#endif // PCAN_TRC_PARSER_H_
