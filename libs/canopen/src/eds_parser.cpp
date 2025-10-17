#include "canopen/eds_parser.h"
#include "canopen/eds_grammar.h"

#include <spdlog/spdlog.h>
#include <lexy/action/match.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy_ext/report_error.hpp>

#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace canopen
{

// ============================================================================
// EDS Parser Implementation
// ============================================================================

const Object* ObjectDictionary::get(uint16_t index) const
{
    auto it = objects.find(index);
    return it == objects.end() ? nullptr : &it->second;
}

const SubObject* ObjectDictionary::get(uint16_t index, uint8_t sub) const
{
    auto it = objects.find(index);
    if (it == objects.end())
    {
        return nullptr;
    }
    auto it2 = it->second.subs.find(sub);

    return it2 == it->second.subs.end() ? nullptr : &it2->second;
}

std::optional<ObjectDictionary> parse_eds(std::string_view text)
{
    auto input = lexy::string_input<lexy::utf8_encoding>(text.data(), text.size());
    auto result = lexy::parse<grammar::eds_document>(input, lexy_ext::report_error);
    if (!result.has_value())
    {
        SPDLOG_ERROR("Failed to parse EDS file");
        return std::nullopt;
    }
    return result.value();
}

} // namespace canopen
