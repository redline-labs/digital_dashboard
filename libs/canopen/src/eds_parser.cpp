// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/eds_parser.h"
#include "canopen/eds_grammar.h"
#include "canopen/pdo_mapping.h"

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/input_location.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <functional>
#include <set>

namespace canopen
{
namespace
{

using Input = lexy::string_input<lexy::utf8_encoding>;

// ============================================================================
// Small string helpers
// ============================================================================

std::string trim(std::string_view text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

bool is_hex_digit(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

// Values inside a section are decimal unless they carry an `0x` prefix. This is
// the opposite of the rule for section names, which are always hexadecimal --
// see the note at the top of eds_ast.h.
std::optional<uint64_t> parse_uint(std::string_view text)
{
    std::string s = trim(text);
    if (s.empty())
    {
        return std::nullopt;
    }

    int base = 10;
    std::string_view digits = s;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        digits = std::string_view(s).substr(2);
    }

    uint64_t value = 0;
    const char* begin = digits.data();
    const char* end = begin + digits.size();
    auto [ptr, ec] = std::from_chars(begin, end, value, base);
    if (ec != std::errc {} || ptr != end)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<int64_t> parse_int(std::string_view text)
{
    std::string s = trim(text);
    if (s.empty())
    {
        return std::nullopt;
    }

    bool negative = false;
    if (s[0] == '-' || s[0] == '+')
    {
        negative = (s[0] == '-');
        s = s.substr(1);
    }

    auto magnitude = parse_uint(s);
    if (!magnitude.has_value())
    {
        return std::nullopt;
    }
    // Signed limits in the wild are small; anything that would wrap is a
    // malformed file rather than a value we should try to represent.
    if (*magnitude > static_cast<uint64_t>(INT64_MAX))
    {
        return std::nullopt;
    }
    const int64_t value = static_cast<int64_t>(*magnitude);
    return negative ? -value : value;
}

// `$NODEID+0x40000180`, `$NODEID - 16`, or a bare `$NODEID`.
std::optional<NodeIdExpr> parse_nodeid_expr(std::string_view text)
{
    std::string s = trim(text);
    constexpr std::string_view marker = "$NODEID";
    if (s.size() < marker.size() || !iequals(std::string_view(s).substr(0, marker.size()), marker))
    {
        return std::nullopt;
    }

    std::string rest = trim(std::string_view(s).substr(marker.size()));
    if (rest.empty())
    {
        return NodeIdExpr { true, 0 };
    }

    const char sign = rest[0];
    if (sign != '+' && sign != '-')
    {
        return std::nullopt;
    }

    auto magnitude = parse_uint(std::string_view(rest).substr(1));
    if (!magnitude.has_value() || *magnitude > static_cast<uint64_t>(INT64_MAX))
    {
        return std::nullopt;
    }

    const int64_t constant = static_cast<int64_t>(*magnitude);
    return NodeIdExpr { true, sign == '+' ? constant : -constant };
}

Value parse_value(std::string_view text)
{
    const std::string s = trim(text);
    if (s.empty())
    {
        return std::monostate {};
    }
    if (auto expr = parse_nodeid_expr(s))
    {
        return *expr;
    }
    if (s[0] == '-')
    {
        if (auto number = parse_int(s))
        {
            return *number;
        }
    }
    if (auto number = parse_uint(s))
    {
        return *number;
    }
    return s;
}

bool parse_bool(std::string_view text)
{
    auto number = parse_uint(text);
    return number.has_value() && *number != 0;
}

// ============================================================================
// Diagnostics
// ============================================================================

// Positions travel through the grammar as opaque pointers into the input; they
// become line and column numbers here, once, and only for lines that actually
// have something to say about them.
class Locator
{
public:
    explicit Locator(const Input& input)
        : input_(input)
    {
    }

    std::pair<int, int> locate(const void* position) const
    {
        if (position == nullptr)
        {
            return { 0, 0 };
        }
        const auto* iterator = static_cast<const LEXY_CHAR8_T*>(position);
        auto location = lexy::get_input_location(input_, iterator);
        return { static_cast<int>(location.line_nr()), static_cast<int>(location.column_nr()) };
    }

private:
    const Input& input_;
};

// Turns one of lexy's structural errors into one of ours. Without this the
// grammar's own failures are invisible: `dsl::terminator` recovers by skipping
// input until it finds something it recognises, so a file that is not an INI
// file at all parses "successfully" into zero sections unless the errors raised
// along the way are collected.
struct GrammarError
{
    using return_type = Diagnostic;

    template <typename ErrorInput, typename Reader, typename Tag>
    Diagnostic operator()(const lexy::error_context<ErrorInput>& context,
                          const lexy::error<Reader, Tag>& error) const
    {
        auto location = lexy::get_input_location(context.input(), error.position());

        std::string what;
        if constexpr (std::is_same_v<Tag, lexy::expected_char_class>)
        {
            what = fmt::format("expected {}", error.name());
        }
        else if constexpr (std::is_same_v<Tag, lexy::expected_literal>)
        {
            what = "expected a literal";
        }
        else if constexpr (std::is_same_v<Tag, lexy::expected_keyword>)
        {
            what = "expected a keyword";
        }
        else
        {
            what = error.message();
        }

        return Diagnostic { Severity::Error, static_cast<int>(location.line_nr()),
                            static_cast<int>(location.column_nr()),
                            fmt::format("{} while reading {}", what, context.production()) };
    }
};

class DiagnosticSink
{
public:
    DiagnosticSink(const Locator& locator, std::vector<Diagnostic>& out)
        : locator_(locator)
        , out_(out)
    {
    }

    void error(const void* position, std::string message)
    {
        add(Severity::Error, position, std::move(message));
    }

    void warn(const void* position, std::string message)
    {
        add(Severity::Warning, position, std::move(message));
    }

private:
    void add(Severity severity, const void* position, std::string message)
    {
        auto [line, column] = locator_.locate(position);
        out_.push_back(Diagnostic { severity, line, column, std::move(message) });
    }

    const Locator& locator_;
    std::vector<Diagnostic>& out_;
};

// ============================================================================
// Section classification
// ============================================================================

enum class SectionKind
{
    FileInfo,
    DeviceInfo,
    Comments,
    DummyUsage,
    MandatoryObjects,
    OptionalObjects,
    ManufacturerObjects,
    Object,
    SubObject,
    // A section we recognise by name and deliberately do not model.
    KnownIgnored,
    Unknown,
};

struct SectionId
{
    SectionKind kind { SectionKind::Unknown };
    uint16_t index { 0 };
    uint8_t sub { 0 };
};

// `[1A00sub3]` -> index 0x1A00, sub 3. Both numbers are hexadecimal: CiA 306
// writes the index as four hex digits and the sub-index as one or two, which
// for subs 0..9 is indistinguishable from decimal and for sub 10 upwards is
// not. Reading either as decimal is the bug that made `od.get(0x1018)` return
// nothing while every test still passed.
std::optional<SectionId> classify_object_section(const std::string& name)
{
    if (name.size() < 4)
    {
        return std::nullopt;
    }
    for (size_t i = 0; i < 4; ++i)
    {
        if (!is_hex_digit(name[i]))
        {
            return std::nullopt;
        }
    }

    auto index = parse_uint("0x" + name.substr(0, 4));
    if (!index.has_value())
    {
        return std::nullopt;
    }

    if (name.size() == 4)
    {
        return SectionId { SectionKind::Object, static_cast<uint16_t>(*index), 0 };
    }

    const std::string tail = name.substr(4);
    if (tail.size() < 4 || !iequals(std::string_view(tail).substr(0, 3), "sub"))
    {
        return std::nullopt;
    }

    const std::string digits = tail.substr(3);
    if (digits.empty() || digits.size() > 2)
    {
        return std::nullopt;
    }
    for (char c : digits)
    {
        if (!is_hex_digit(c))
        {
            return std::nullopt;
        }
    }

    auto sub = parse_uint("0x" + digits);
    if (!sub.has_value() || *sub > 0xFF)
    {
        return std::nullopt;
    }

    return SectionId { SectionKind::SubObject, static_cast<uint16_t>(*index),
                       static_cast<uint8_t>(*sub) };
}

SectionId classify_section(const std::string& rawName)
{
    const std::string name = trim(rawName);

    if (iequals(name, "FileInfo")) return { SectionKind::FileInfo, 0, 0 };
    if (iequals(name, "DeviceInfo")) return { SectionKind::DeviceInfo, 0, 0 };
    if (iequals(name, "Comments")) return { SectionKind::Comments, 0, 0 };
    if (iequals(name, "DummyUsage")) return { SectionKind::DummyUsage, 0, 0 };
    if (iequals(name, "MandatoryObjects")) return { SectionKind::MandatoryObjects, 0, 0 };
    if (iequals(name, "OptionalObjects")) return { SectionKind::OptionalObjects, 0, 0 };
    if (iequals(name, "ManufacturerObjects")) return { SectionKind::ManufacturerObjects, 0, 0 };

    // Named in CiA 306 but carrying nothing this library acts on. Listed so a
    // conforming file does not produce a warning on every run.
    static const char* const knownIgnored[] = {
        "VirtualDeviceDefinitions", "StandardDataTypes", "SupportedModules",
        "DeviceComissioning",       "DeviceCommissioning", "Tools",
    };
    for (const char* candidate : knownIgnored)
    {
        if (iequals(name, candidate))
        {
            return { SectionKind::KnownIgnored, 0, 0 };
        }
    }

    if (auto object = classify_object_section(name))
    {
        return *object;
    }

    return { SectionKind::Unknown, 0, 0 };
}

// ============================================================================
// Section body reading
// ============================================================================

struct Entry
{
    std::string key;
    std::string value;
    const void* position { nullptr };
};

// Splits a section's raw lines into key/value entries, reporting anything that
// is neither a comment, a blank line, nor `key=value`. A bad line costs a
// diagnostic and nothing else -- the rest of the section is still read.
std::vector<Entry> read_entries(const grammar::RawSection& section, DiagnosticSink& diagnostics)
{
    std::vector<Entry> entries;
    entries.reserve(section.lines.size());

    for (const auto& line : section.lines)
    {
        const std::string text = trim(line.text);
        if (text.empty() || text[0] == ';' || text[0] == '#')
        {
            continue;
        }

        const size_t equals = text.find('=');
        if (equals == std::string::npos)
        {
            diagnostics.error(line.position,
                              fmt::format("expected 'key=value', got '{}'", text));
            continue;
        }

        Entry entry;
        entry.key = trim(std::string_view(text).substr(0, equals));
        entry.value = trim(std::string_view(text).substr(equals + 1));
        entry.position = line.position;

        if (entry.key.empty())
        {
            diagnostics.error(line.position, "key is empty");
            continue;
        }

        entries.push_back(std::move(entry));
    }

    return entries;
}

const Entry* find(const std::vector<Entry>& entries, std::string_view key)
{
    for (const auto& entry : entries)
    {
        if (iequals(entry.key, key))
        {
            return &entry;
        }
    }
    return nullptr;
}

// ============================================================================
// Object and sub-object bodies
// ============================================================================

// The keys that describe a single entry. A VAR object section and a subN
// section carry exactly the same set, which is why one function reads both --
// the previous parser read them only for subN sections, leaving every VAR
// object (0x1000, 0x1001, 0x1017, ...) with nothing but a name.
void read_entry_body(const std::vector<Entry>& entries, SubObject& out,
                     DiagnosticSink& diagnostics)
{
    const void* defaultValuePosition = nullptr;

    for (const auto& entry : entries)
    {
        if (iequals(entry.key, "ParameterName"))
        {
            out.parameterName = entry.value;
        }
        else if (iequals(entry.key, "ObjectType"))
        {
            if (auto raw = parse_uint(entry.value))
            {
                out.objectCode = static_cast<ObjectCode>(*raw);
            }
            else if (!entry.value.empty())
            {
                diagnostics.error(entry.position,
                                  fmt::format("ObjectType is not a number: '{}'", entry.value));
            }
        }
        else if (iequals(entry.key, "DataType"))
        {
            if (auto raw = parse_uint(entry.value))
            {
                if (!is_known_data_type(static_cast<uint16_t>(*raw)))
                {
                    diagnostics.warn(entry.position,
                                     fmt::format("unknown DataType 0x{:04X}", *raw));
                }
                out.dataType = static_cast<DataType>(*raw);
            }
            else if (!entry.value.empty())
            {
                diagnostics.error(entry.position,
                                  fmt::format("DataType is not a number: '{}'", entry.value));
            }
        }
        else if (iequals(entry.key, "LowLimit") || iequals(entry.key, "HighLimit"))
        {
            // An empty limit is how an EDS says "unbounded", and it is written
            // that way throughout the Grayhill file. Only a non-empty value
            // that fails to parse is an error.
            if (entry.value.empty())
            {
                continue;
            }
            auto limit = parse_int(entry.value);
            if (!limit.has_value())
            {
                diagnostics.error(entry.position,
                                  fmt::format("{} is not a number: '{}'", entry.key, entry.value));
                continue;
            }
            if (iequals(entry.key, "LowLimit"))
            {
                out.lowLimit = *limit;
            }
            else
            {
                out.highLimit = *limit;
            }
        }
        else if (iequals(entry.key, "AccessType"))
        {
            if (iequals(entry.value, "ro")) out.access = AccessType::RO;
            else if (iequals(entry.value, "wo")) out.access = AccessType::WO;
            else if (iequals(entry.value, "rw")) out.access = AccessType::RW;
            else if (iequals(entry.value, "rwr")) out.access = AccessType::RWR;
            else if (iequals(entry.value, "rww")) out.access = AccessType::RWW;
            else if (iequals(entry.value, "const")) out.access = AccessType::CONST;
            else
            {
                // Not a warning: leaving the RO default in place would make a
                // writable object look read-only, and the reconfiguration tool
                // decides what it may write from exactly this field.
                diagnostics.error(entry.position,
                                  fmt::format("unknown AccessType '{}'", entry.value));
            }
        }
        else if (iequals(entry.key, "DefaultValue"))
        {
            Value value = parse_value(entry.value);
            if (!std::holds_alternative<std::monostate>(value))
            {
                out.defaultValue = std::move(value);
                defaultValuePosition = entry.position;
            }
        }
        else if (iequals(entry.key, "PDOMapping"))
        {
            out.pdoMappable = parse_bool(entry.value);
        }
        else if (iequals(entry.key, "ObjFlags"))
        {
            if (auto flags = parse_uint(entry.value))
            {
                out.objFlags = static_cast<uint32_t>(*flags);
            }
        }
    }

    // A default that is neither a number nor a $NODEID expression is text, and
    // for a string-typed object that is exactly right -- 0x1008's default
    // really is the word "manufacturer". Only complain when a numeric object
    // has a default that could not be read as a number, which is the case that
    // silently produces a zero somewhere downstream. Checked after the loop
    // because DataType is not required to appear before DefaultValue.
    if (defaultValuePosition != nullptr && out.defaultValue.has_value())
    {
        if (const auto* text = std::get_if<std::string>(&*out.defaultValue))
        {
            const bool isTextType = out.dataType == DataType::VisibleString
                || out.dataType == DataType::OctetString
                || out.dataType == DataType::UnicodeString || out.dataType == DataType::Domain;
            if (!isTextType)
            {
                diagnostics.error(defaultValuePosition,
                                  fmt::format("DefaultValue '{}' is not a number or a $NODEID "
                                              "expression, but the object is {}",
                                              *text, to_string(out.dataType)));
            }
        }
    }
}

// ============================================================================
// Metadata sections
// ============================================================================

void read_file_info(const std::vector<Entry>& entries, FileInfo& out)
{
    for (const auto& entry : entries)
    {
        if (iequals(entry.key, "FileName")) out.fileName = entry.value;
        else if (iequals(entry.key, "Description")) out.description = entry.value;
        else if (iequals(entry.key, "CreatedBy")) out.createdBy = entry.value;
        else if (iequals(entry.key, "ModifiedBy")) out.modifiedBy = entry.value;
        else if (iequals(entry.key, "EDSVersion")) out.edsVersion = entry.value;
        else if (iequals(entry.key, "FileVersion"))
        {
            if (auto n = parse_uint(entry.value)) out.fileVersion = static_cast<uint32_t>(*n);
        }
        else if (iequals(entry.key, "FileRevision"))
        {
            if (auto n = parse_uint(entry.value)) out.fileRevision = static_cast<uint32_t>(*n);
        }
    }
}

void read_device_info(const std::vector<Entry>& entries, DeviceInfo& out)
{
    // The eight rates CiA 301 defines. Absent means "not declared", which the
    // file distinguishes from `BaudRate_10=0` meaning "declared unsupported".
    static constexpr uint32_t rates[] = { 10, 20, 50, 125, 250, 500, 800, 1000 };

    for (const auto& entry : entries)
    {
        if (iequals(entry.key, "VendorName") || iequals(entry.key, "Vendorname"))
        {
            out.vendorName = entry.value;
        }
        else if (iequals(entry.key, "ProductName")) out.productName = entry.value;
        else if (iequals(entry.key, "OrderCode")) out.orderCode = entry.value;
        else if (iequals(entry.key, "VendorNumber"))
        {
            if (auto n = parse_uint(entry.value)) out.vendorNumber = static_cast<uint32_t>(*n);
        }
        else if (iequals(entry.key, "ProductNumber"))
        {
            if (auto n = parse_uint(entry.value)) out.productNumber = static_cast<uint32_t>(*n);
        }
        else if (iequals(entry.key, "RevisionNumber"))
        {
            if (auto n = parse_uint(entry.value)) out.revisionNumber = static_cast<uint32_t>(*n);
        }
        else if (iequals(entry.key, "NrOfRXPDO"))
        {
            if (auto n = parse_uint(entry.value)) out.nrOfRxPdo = static_cast<uint8_t>(*n);
        }
        else if (iequals(entry.key, "NrOfTXPDO"))
        {
            if (auto n = parse_uint(entry.value)) out.nrOfTxPdo = static_cast<uint8_t>(*n);
        }
        else if (iequals(entry.key, "LSS_Supported")) out.lssSupported = parse_bool(entry.value);
        else if (iequals(entry.key, "SimpleBootUpMaster"))
        {
            out.simpleBootUpMaster = parse_bool(entry.value);
        }
        else if (iequals(entry.key, "SimpleBootUpSlave"))
        {
            out.simpleBootUpSlave = parse_bool(entry.value);
        }
        else if (iequals(entry.key, "DynamicChannelsSupported"))
        {
            out.dynamicChannelsSupported = parse_bool(entry.value);
        }
        else if (iequals(entry.key, "GroupMessaging"))
        {
            out.groupMessaging = parse_bool(entry.value);
        }
        else if (iequals(entry.key, "Granularity"))
        {
            if (auto n = parse_uint(entry.value)) out.granularity = static_cast<uint8_t>(*n);
        }
        else if (iequals(entry.key, "CompactPDO"))
        {
            if (auto n = parse_uint(entry.value)) out.compactPdo = static_cast<uint32_t>(*n);
        }
        else
        {
            for (uint32_t rate : rates)
            {
                if (iequals(entry.key, fmt::format("BaudRate_{}", rate)))
                {
                    out.supportedBitrates[rate] = parse_bool(entry.value);
                    break;
                }
            }
        }
    }
}

// `[Comments]` and the three object-list sections share a shape: a count key
// followed by numbered entries. The count is checked against what is actually
// there, because a stale count is a common hand-edit mistake and a silent one.
void read_numbered_list(const std::vector<Entry>& entries, std::string_view countKey,
                        std::string_view prefix, DiagnosticSink& diagnostics,
                        const std::function<void(const Entry&)>& consume)
{
    size_t found = 0;
    for (const auto& entry : entries)
    {
        if (iequals(entry.key, countKey))
        {
            continue;
        }
        if (!prefix.empty())
        {
            if (entry.key.size() <= prefix.size()
                || !iequals(std::string_view(entry.key).substr(0, prefix.size()), prefix))
            {
                continue;
            }
        }
        consume(entry);
        ++found;
    }

    const Entry* count = find(entries, countKey);
    if (count != nullptr)
    {
        auto declared = parse_uint(count->value);
        if (declared.has_value() && *declared != found)
        {
            diagnostics.error(count->position,
                              fmt::format("{}={} but {} entries are present", countKey, *declared,
                                          found));
        }
    }
}

void read_object_list(const std::vector<Entry>& entries, std::vector<uint16_t>& out,
                      DiagnosticSink& diagnostics)
{
    read_numbered_list(entries, "SupportedObjects", "", diagnostics,
                       [&](const Entry& entry)
                       {
                           auto index = parse_uint(entry.value);
                           if (!index.has_value() || *index > 0xFFFF)
                           {
                               diagnostics.error(entry.position,
                                                 fmt::format("'{}' is not an object index",
                                                             entry.value));
                               return;
                           }
                           out.push_back(static_cast<uint16_t>(*index));
                       });
}

} // namespace

// ============================================================================
// Public entry points
// ============================================================================

std::string to_string(const Diagnostic& diagnostic)
{
    const char* severity = diagnostic.severity == Severity::Error ? "error" : "warning";
    if (diagnostic.line == 0)
    {
        return fmt::format("{}: {}", severity, diagnostic.message);
    }
    return fmt::format("{}:{}: {}: {}", diagnostic.line, diagnostic.column, severity,
                       diagnostic.message);
}

bool ParseResult::ok() const
{
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

ParseResult parse_eds(std::string_view text)
{
    ParseResult result;

    auto input = lexy::string_input<lexy::utf8_encoding>(text.data(), text.size());
    Locator locator(input);
    DiagnosticSink diagnostics(locator, result.diagnostics);

    // The grammar recognises "bracketed name, then lines" and nothing more, so
    // in practice it only fails on input that is not an INI file at all.
    auto parsed = lexy::parse<grammar::document>(
        input, lexy::collect<std::vector<Diagnostic>>(GrammarError {}));

    for (auto& diagnostic : parsed.errors())
    {
        result.diagnostics.push_back(std::move(diagnostic));
    }

    if (!parsed.has_value())
    {
        if (result.ok())
        {
            result.diagnostics.push_back(
                Diagnostic { Severity::Error, 0, 0, "file is not in EDS (INI) form" });
        }
        return result;
    }

    const grammar::RawDocument document = std::move(parsed).value();

    // Sections are applied in file order, but a subN section may precede its
    // parent object section in a malformed file. Creating the parent lazily
    // here and stamping its index means such a file yields a correctly-indexed
    // object rather than one silently indexed 0.
    auto object_for = [&](uint16_t index) -> Object&
    {
        Object& object = result.od.objects[index];
        object.index = index;
        return object;
    };

    std::set<std::string> seenSections;

    for (const auto& section : document)
    {
        const std::string name = trim(section.name);
        if (!seenSections.insert(name).second)
        {
            diagnostics.error(section.position, fmt::format("duplicate section [{}]", name));
        }

        const std::vector<Entry> entries = read_entries(section, diagnostics);
        const SectionId id = classify_section(name);

        switch (id.kind)
        {
        case SectionKind::FileInfo:
            read_file_info(entries, result.od.fileInfo);
            break;

        case SectionKind::DeviceInfo:
            read_device_info(entries, result.od.deviceInfo);
            break;

        case SectionKind::Comments:
            read_numbered_list(entries, "Lines", "Line", diagnostics,
                               [&](const Entry& entry)
                               { result.od.comments.push_back(entry.value); });
            break;

        case SectionKind::DummyUsage:
            for (const auto& entry : entries)
            {
                // Keys are `Dummy0001`..`Dummy0007`: the data type index the
                // device will accept as a PDO mapping placeholder.
                if (entry.key.size() > 5
                    && iequals(std::string_view(entry.key).substr(0, 5), "Dummy"))
                {
                    if (auto type = parse_uint("0x" + entry.key.substr(5)))
                    {
                        result.od.dummyUsage[static_cast<uint16_t>(*type)]
                            = parse_bool(entry.value);
                    }
                }
            }
            break;

        case SectionKind::MandatoryObjects:
            read_object_list(entries, result.od.mandatoryObjects, diagnostics);
            break;

        case SectionKind::OptionalObjects:
            read_object_list(entries, result.od.optionalObjects, diagnostics);
            break;

        case SectionKind::ManufacturerObjects:
            read_object_list(entries, result.od.manufacturerObjects, diagnostics);
            break;

        case SectionKind::Object:
        {
            Object& object = object_for(id.index);

            SubObject body;
            body.subIndex = 0;
            read_entry_body(entries, body, diagnostics);

            object.parameterName = body.parameterName;
            object.objectCode = body.objectCode;
            object.objFlags = body.objFlags;

            if (const Entry* subNumber = find(entries, "SubNumber"))
            {
                if (auto n = parse_uint(subNumber->value))
                {
                    object.declaredSubNumber = static_cast<uint8_t>(*n);
                }
                else
                {
                    diagnostics.error(subNumber->position,
                                      fmt::format("SubNumber is not a number: '{}'",
                                                  subNumber->value));
                }
            }

            // A VAR carries its value in the object section itself. Storing it
            // at sub 0 means get(index, 0) answers for VAR and ARRAY alike.
            if (object.objectCode == ObjectCode::Var)
            {
                object.subs[0] = std::move(body);
            }
            break;
        }

        case SectionKind::SubObject:
        {
            Object& object = object_for(id.index);
            SubObject body;
            body.subIndex = id.sub;
            read_entry_body(entries, body, diagnostics);
            object.subs[id.sub] = std::move(body);
            break;
        }

        case SectionKind::KnownIgnored:
            break;

        case SectionKind::Unknown:
            diagnostics.warn(section.position,
                             fmt::format("unrecognised section [{}]; ignored", name));
            break;
        }
    }

    return result;
}

std::vector<Diagnostic> validate(const ObjectDictionary& od)
{
    std::vector<Diagnostic> out;
    auto report = [&](Severity severity, std::string message)
    { out.push_back(Diagnostic { severity, 0, 0, std::move(message) }); };

    // --- CiA 301 floor -----------------------------------------------------
    // The only objects a conforming device must implement. There is no profile
    // inheritance, so everything else has to be declared explicitly and their
    // absence is the file's statement, not an omission to be filled in.
    for (uint16_t index : { uint16_t { 0x1000 }, uint16_t { 0x1001 }, uint16_t { 0x1018 } })
    {
        if (od.get(index) == nullptr)
        {
            report(Severity::Error,
                   fmt::format("mandatory object 0x{:04X} is missing from the file", index));
        }
    }

    // --- declared vs present, in both directions ---------------------------
    std::set<uint16_t> declared;
    for (const auto* list : { &od.mandatoryObjects, &od.optionalObjects, &od.manufacturerObjects })
    {
        for (uint16_t index : *list)
        {
            declared.insert(index);
            if (od.get(index) == nullptr)
            {
                report(Severity::Error,
                       fmt::format("0x{:04X} is listed as supported but has no [{:04X}] section",
                                   index, index));
            }
        }
    }
    for (const auto& [index, object] : od.objects)
    {
        if (declared.find(index) == declared.end())
        {
            report(Severity::Error,
                   fmt::format("[{:04X}] '{}' is present but not listed in MandatoryObjects, "
                               "OptionalObjects or ManufacturerObjects",
                               index, object.parameterName));
        }
    }

    for (const auto& [index, object] : od.objects)
    {
        const bool isVar = object.objectCode == ObjectCode::Var;

        // --- SubNumber against reality -------------------------------------
        // A VAR has no subN sections; its `subs` holds the synthetic sub 0.
        if (!isVar && object.declaredSubNumber.has_value())
        {
            if (*object.declaredSubNumber != object.subs.size())
            {
                report(Severity::Error,
                       fmt::format("[{:04X}] SubNumber={} but {} sub-sections are present", index,
                                   *object.declaredSubNumber, object.subs.size()));
            }
        }

        // --- sub 0 "number of entries" against reality ---------------------
        if (!isVar)
        {
            const SubObject* countEntry = object.subs.count(0) != 0 ? &object.subs.at(0) : nullptr;
            if (countEntry == nullptr)
            {
                report(Severity::Error,
                       fmt::format("[{:04X}] is an {} but has no sub 0 entry count", index,
                                   object.objectCode == ObjectCode::Array ? "ARRAY" : "RECORD"));
            }
            else if (countEntry->defaultValue.has_value())
            {
                if (const auto* declaredCount
                    = std::get_if<uint64_t>(&*countEntry->defaultValue))
                {
                    // sub 0 counts the entries excluding itself. The highest
                    // sub-index is the more useful reading for a sparse object
                    // like 0x1800, which skips sub 4.
                    const uint8_t highest = object.subs.rbegin()->first;
                    const size_t present = object.subs.size() - 1;
                    if (*declaredCount != present && *declaredCount != highest)
                    {
                        report(Severity::Error,
                               fmt::format("[{:04X}sub0]={} matches neither the {} entries "
                                           "present nor the highest sub-index {}",
                                           index, *declaredCount, present, highest));
                    }
                }
            }
        }

        // --- defaults inside their own limits ------------------------------
        for (const auto& [sub, entry] : object.subs)
        {
            if (!entry.defaultValue.has_value())
            {
                continue;
            }
            // A $NODEID default is only a number once a node ID is chosen, and
            // COB-ID limits are not expressed in the file anyway.
            const auto* number = std::get_if<uint64_t>(&*entry.defaultValue);
            if (number == nullptr)
            {
                continue;
            }
            const int64_t value = static_cast<int64_t>(*number);
            if (entry.lowLimit.has_value() && value < *entry.lowLimit)
            {
                report(Severity::Error,
                       fmt::format("[{:04X}sub{:X}] DefaultValue {} is below LowLimit {}", index,
                                   sub, value, *entry.lowLimit));
            }
            if (entry.highLimit.has_value() && value > *entry.highLimit)
            {
                report(Severity::Error,
                       fmt::format("[{:04X}sub{:X}] DefaultValue {} is above HighLimit {}", index,
                                   sub, value, *entry.highLimit));
            }
            if (entry.lowLimit.has_value() && entry.highLimit.has_value()
                && *entry.lowLimit > *entry.highLimit)
            {
                report(Severity::Error,
                       fmt::format("[{:04X}sub{:X}] LowLimit {} is above HighLimit {}", index, sub,
                                   *entry.lowLimit, *entry.highLimit));
            }
        }
    }

    // --- PDO mappings ------------------------------------------------------
    for (const auto& [index, object] : od.objects)
    {
        if (!is_pdo_mapping_index(index))
        {
            continue;
        }
        (void)object;

        auto mapping = read_pdo_mapping(od, index);
        for (const auto& problem : mapping.problems)
        {
            report(Severity::Error, problem);
        }
        if (mapping.totalBits > 64)
        {
            report(Severity::Error,
                   fmt::format("[{:04X}] maps {} bits; a CAN frame holds 64", index,
                               mapping.totalBits));
        }
    }

    return out;
}

} // namespace canopen
