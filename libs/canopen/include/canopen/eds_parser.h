#ifndef CANOPEN_EDS_PARSER_H
#define CANOPEN_EDS_PARSER_H

#include "canopen/eds_ast.h"

#include <optional>
#include <string>
#include <string_view>

namespace canopen
{

// Parse an EDS file from text and return the ObjectDictionary on success.
std::optional<ObjectDictionary> parse_eds(std::string_view text);

} // namespace canopen

#endif // CANOPEN_EDS_PARSER_H


