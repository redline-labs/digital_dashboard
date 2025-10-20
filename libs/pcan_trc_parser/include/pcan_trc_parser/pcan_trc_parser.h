#ifndef PCAN_TRC_PARSER_H_
#define PCAN_TRC_PARSER_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "helpers/can_frame.h"

namespace pcan_trc_parser
{

// Parse a PCAN .trc file and invoke the callback for each parsed frame.
// The callback should return true to continue parsing, or false to stop early.
// Returns the number of frames successfully delivered to the callback.
std::size_t parse_file(const std::string& path,
                       const std::function<bool(const helpers::CanFrame&)>& on_frame);

// Parse PCAN .trc contents provided as a NUL-terminated C-string.
// Uses the same callback interface as parse_file. Returns delivered frames.
std::size_t parse_string(const char* trc_contents,
                         const std::function<bool(const helpers::CanFrame&)>& on_frame);

}  // namespace pcan_trc_parser

#endif // PCAN_TRC_PARSER_H_
