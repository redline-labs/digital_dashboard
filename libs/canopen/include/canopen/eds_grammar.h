#ifndef CANOPEN_EDS_GRAMMAR_H
#define CANOPEN_EDS_GRAMMAR_H

#include <lexy/dsl.hpp>
#include <lexy/action/match.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy_ext/report_error.hpp>

#include "canopen/eds_ast.h"

#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace canopen
{

// Helper type aliases for section K/V collection
using KeyValue = std::pair<std::string, std::string>;

namespace grammar
{

namespace dsl = lexy::dsl;

// Helpers used by multiple productions
static inline std::optional<uint64_t> parse_uint_str(const std::string& s)
{
    if (s.empty()) return std::nullopt;
    try
    {
        if (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X"))
            return std::stoull(s, nullptr, 16);
        else
            return std::stoull(s, nullptr, 10);
    }
    catch (...) { return std::nullopt; }
}

static inline bool iequals_local(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Integer literal: decimal or 0x/0X-prefixed hex
struct uint_literal : lexy::token_production
{
    static constexpr auto rule =
        (LEXY_LIT("0x") | LEXY_LIT("0X")) >> dsl::integer<std::uint64_t, dsl::hex>
        | dsl::integer<std::uint64_t>;
    static constexpr auto value = lexy::as_integer<std::uint64_t>;
};

// Token for section name content (everything except ] and newline)
struct section_name_token : lexy::token_production
{
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_(dsl::ascii::character - dsl::lit_c<']'> - dsl::ascii::newline)));
    static constexpr auto value = lexy::as_string<std::string>;
};

// Token for key (at least one character before = and newline)
struct key_token : lexy::token_production
{
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_one(dsl::ascii::character - dsl::lit_c<'='> - dsl::ascii::newline)));
    static constexpr auto value = lexy::as_string<std::string>;
};

// Token for value (everything until newline/eof)
struct value_token : lexy::token_production
{
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_(dsl::ascii::character - dsl::ascii::newline)));
    static constexpr auto value = lexy::as_string<std::string>;
};

// Comment line: ; or # followed by anything → ignored as empty K/V
struct comment_line
{
    static constexpr auto rule = 
        (dsl::lit_c<';'> | dsl::lit_c<'#'>) >>
        dsl::while_(dsl::ascii::character - dsl::ascii::newline);
    static constexpr auto value = lexy::constant(KeyValue{"", ""});
};

// Generic key=value pair capturing raw strings → KeyValue
struct key_value_pair
{
    static constexpr auto rule = 
        dsl::p<key_token> + dsl::lit_c<'='> + dsl::p<value_token>;
    static constexpr auto value = lexy::callback<KeyValue>(
        [](std::string&& key, std::string&& value) {
            auto trim = [](std::string& s)
            {
                size_t b = 0, e = s.size();
                while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
                while (e > b && std::isspace(static_cast<unsigned char>(s[e-1]))) --e;
                s = s.substr(b, e - b);
            };
            trim(key);
            trim(value);
            return KeyValue{std::move(key), std::move(value)};
        }
    );
};

// Blank line (empty or only whitespace)
struct blank_line
{
    static constexpr auto rule = dsl::peek(dsl::newline | dsl::eof) >> dsl::return_;
    static constexpr auto value = lexy::constant(KeyValue{"", ""});
};

// One body line inside a section: blank | comment | key=value
struct body_line
{
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule =
          (dsl::peek(dsl::lit_c<';'> | dsl::lit_c<'#'>) >> dsl::p<comment_line>)
        | (dsl::peek(dsl::ascii::character - dsl::lit_c<'['> - dsl::lit_c<';'> - dsl::lit_c<'#'> - dsl::ascii::newline) >> dsl::p<key_value_pair>)
        | dsl::p<blank_line>;
    static constexpr auto value = lexy::forward<KeyValue>;
};

// Section bodies: helper to read lines until next section header or EOF
struct section_body_lines
{
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = dsl::terminator(dsl::peek(dsl::lit_c<'['>) | dsl::eof)
                                     .opt_list(dsl::p<body_line>, dsl::trailing_sep(dsl::newline));
    static constexpr auto value = lexy::as_list<std::vector<KeyValue>>;
};

// [FileInfo] section → produces Section + its lines
struct fileinfo_section
{
    struct result { canopen::FileInfo fileInfo; };
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = LEXY_LIT("[") + LEXY_LIT("FileInfo") + LEXY_LIT("]") + dsl::newline + dsl::p<section_body_lines>;
    static constexpr auto value = lexy::callback<result>(
        [](std::vector<KeyValue>&& lines) {
            canopen::FileInfo fi{};
            for (auto& [k, v] : lines)
            {
                if (k == "FileName") fi.fileName = v;
                else if (k == "Description") fi.description = v;
                else if (k == "CreatedBy") fi.createdBy = v;
            }
            return result{fi};
        }
    );
};

// [DeviceInfo] section → produces Section + its lines
struct deviceinfo_section
{
    struct result { canopen::DeviceInfo deviceInfo; };
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = LEXY_LIT("[") + LEXY_LIT("DeviceInfo") + LEXY_LIT("]") + dsl::newline + dsl::p<section_body_lines>;
    static constexpr auto value = lexy::callback<result>(
        [](std::vector<KeyValue>&& lines) {
            canopen::DeviceInfo di{};
            for (auto& [k, v] : lines)
            {
                if (k == "Vendorname") di.vendorName = v;
                else if (k == "VendorNumber") { if (auto n = parse_uint_str(v)) di.vendorNumber = static_cast<uint32_t>(*n); }
                else if (k == "ProductName") di.productName = v;
                else if (k == "ProductNumber") { if (auto n = parse_uint_str(v)) di.productNumber = static_cast<uint32_t>(*n); }
                else if (k == "RevisionNumber") { if (auto n = parse_uint_str(v)) di.revisionNumber = static_cast<uint32_t>(*n); }
                else if (k == "NrOfRXPDO") { if (auto n = parse_uint_str(v)) di.nrOfRxPdo = static_cast<uint8_t>(*n); }
                else if (k == "NrOfTXPDO") { if (auto n = parse_uint_str(v)) di.nrOfTxPdo = static_cast<uint8_t>(*n); }
            }
            return result{di};
        }
    );
};

// [1234] section
struct object_section
{
    struct result { std::uint16_t index; canopen::Object object; };
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = LEXY_LIT("[") + dsl::p<uint_literal> + LEXY_LIT("]") + dsl::newline + dsl::p<section_body_lines>;
    static constexpr auto value = lexy::callback<result>(
        [](std::uint64_t index, std::vector<KeyValue>&& lines) {
            canopen::Object obj{};
            obj.index = static_cast<std::uint16_t>(index);
            for (auto& [k, v] : lines)
            {
                if (k == "ParameterName") obj.parameterName = v;
                else if (k == "ObjectType") { if (auto n = parse_uint_str(v)) obj.objectType = static_cast<uint8_t>(*n); }
            }
            return result{static_cast<std::uint16_t>(index), obj};
        }
    );
};

// [1234subN] section
struct subobject_section
{
    struct result { std::uint16_t index; std::uint8_t sub; canopen::SubObject subobj; };
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = LEXY_LIT("[") + dsl::p<uint_literal> + LEXY_LIT("sub") + dsl::p<uint_literal> + LEXY_LIT("]") + dsl::newline + dsl::p<section_body_lines>;
    static constexpr auto value = lexy::callback<result>(
        [](std::uint64_t index, std::uint64_t sub, std::vector<KeyValue>&& lines) {
            canopen::SubObject so{};
            so.subIndex = static_cast<std::uint8_t>(sub);
            for (auto& [k, v] : lines)
            {
                if (k == "ParameterName") so.parameterName = v;
                else if (k == "DataType") { if (auto n = parse_uint_str(v)) so.dataType = static_cast<canopen::DataType>(*n); }
                else if (k == "LowLimit") { if (auto n = parse_uint_str(v)) so.lowLimit = static_cast<int64_t>(*n); }
                else if (k == "HighLimit") { if (auto n = parse_uint_str(v)) so.highLimit = static_cast<int64_t>(*n); }
                else if (k == "AccessType")
                {
                    if (iequals_local(v, "ro")) so.access = canopen::AccessType::RO;
                    else if (iequals_local(v, "rw")) so.access = canopen::AccessType::RW;
                    else if (iequals_local(v, "rww")) so.access = canopen::AccessType::RWW;
                    else if (iequals_local(v, "const")) so.access = canopen::AccessType::CONST;
                }
                else if (k == "DefaultValue")
                {
                    if (auto n = parse_uint_str(v)) so.defaultValue = static_cast<uint64_t>(*n);
                    else so.defaultValue = v;
                }
                else if (k == "PDOMapping")
                {
                    if (auto n = parse_uint_str(v)) so.pdoMappable = (*n != 0);
                }
            }
            return result{static_cast<std::uint16_t>(index), static_cast<std::uint8_t>(sub), so};
        }
    );
};

// Generic named section fallback
struct generic_section
{
    struct result { std::string name; };
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = LEXY_LIT("[") + dsl::p<section_name_token> + LEXY_LIT("]") + dsl::newline + dsl::p<section_body_lines>;
    static constexpr auto value = lexy::callback<result>(
        [](std::string&& name, std::vector<KeyValue>&&) {
            return result{std::move(name)};
        }
    );
};

// A section is chosen by branch conditions using peek
using SectionVariant = std::variant<
    fileinfo_section::result,
    deviceinfo_section::result,
    object_section::result,
    subobject_section::result,
    generic_section::result
>;

struct eds_section
{
    static constexpr auto rule =
          (dsl::peek(LEXY_LIT("[") + LEXY_LIT("FileInfo") + LEXY_LIT("]")) >> dsl::p<fileinfo_section>)
        | (dsl::peek(LEXY_LIT("[") + LEXY_LIT("DeviceInfo") + LEXY_LIT("]")) >> dsl::p<deviceinfo_section>)
        | (dsl::peek(LEXY_LIT("[") + dsl::p<uint_literal> + LEXY_LIT("sub")) >> dsl::p<subobject_section>)
        | (dsl::peek(LEXY_LIT("[") + dsl::p<uint_literal> + LEXY_LIT("]")) >> dsl::p<object_section>)
        | (dsl::peek(LEXY_LIT("[")) >> dsl::p<generic_section>);
    static constexpr auto value = lexy::forward<SectionVariant>;
};

// Collect all sections into a vector
struct sections_list
{
    static constexpr auto whitespace = dsl::ascii::blank | dsl::newline;
    static constexpr auto rule = dsl::terminator(dsl::eof).opt_list(dsl::p<eds_section>);
    static constexpr auto value = lexy::as_list<std::vector<SectionVariant>>;
};

// The entire EDS file: list of sections → build ObjectDictionary
struct eds_document
{
    static constexpr auto whitespace = dsl::ascii::blank | dsl::newline;
    static constexpr auto rule = dsl::p<sections_list>;
    static constexpr auto value = lexy::callback<canopen::ObjectDictionary>(
        [](std::vector<SectionVariant>&& sections) {
            canopen::ObjectDictionary od{};

            for (auto& s : sections)
            {
                if (std::holds_alternative<fileinfo_section::result>(s))
                {
                    auto sec = std::get<fileinfo_section::result>(std::move(s));
                    od.fileInfo = sec.fileInfo;
                }
                else if (std::holds_alternative<deviceinfo_section::result>(s))
                {
                    auto sec = std::get<deviceinfo_section::result>(std::move(s));
                    od.deviceInfo = sec.deviceInfo;
                }
                else if (std::holds_alternative<object_section::result>(s))
                {
                    auto sec = std::get<object_section::result>(std::move(s));
                    od.objects[sec.index] = std::move(sec.object);
                }
                else if (std::holds_alternative<subobject_section::result>(s))
                {
                    auto sec = std::get<subobject_section::result>(std::move(s));
                    od.objects[sec.index].subs[sec.sub] = std::move(sec.subobj);
                }
                else if (std::holds_alternative<generic_section::result>(s))
                {
                    // Unknown section: ignored
                }
            }

            return od;
        }
    );
};

}  // namespace grammar
}  // namespace canopen

#endif // CANOPEN_EDS_GRAMMAR_H