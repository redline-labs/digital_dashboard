// SPDX-License-Identifier: GPL-3.0-or-later
//
// There is no parser-combinator grammar here any more, and that is deliberate.
//
// This file used to be a lexy grammar. A grammar is the right tool when the
// shape of the input is fixed and known at compile time, and that is exactly
// what a .trc file is not: the file declares its own column order in a
// `;$COLUMNS=` line, and which of those columns a line actually carries depends
// on the record's type. The old grammar hard-coded one v2.0 layout, so a v2.1
// file -- same decade, same vendor, one extra column -- failed every line. It
// also failed the *file* rather than the line, so a single bad row anywhere
// returned zero frames, and it reported that by printing to stderr from inside
// a library.
//
// What replaced it is a tokeniser and a walk over the declared column list. It
// is longer, but every one of those properties is now a thing this file can
// state and a test can pin.

#include "can_trc/trc.h"

#include "can/dlc.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <limits>

namespace can::trc
{

namespace
{

// Past this many complaints a file is not a trace with a few bad rows, it is
// the wrong file, and one log line per row helps nobody.
constexpr uint64_t kMaxWarnings = 20;

// Days between the OLE Automation epoch (1899-12-30) and the UNIX epoch.
constexpr double kOleUnixEpochDays = 25569.0;
constexpr double kSecondsPerDay = 86400.0;

// helpers::CanFrame carries 64 bytes. J1939 traces may declare up to 1785, and
// a v3.0 XL frame up to 2048; neither fits, and truncating one silently is the
// bug this rewrite exists to remove.
constexpr size_t kMaxPayload = 64;

bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Every token on a line, plus where each began, because an EV record's text is
// "the rest of the line" and needs the offset rather than the pieces.
struct Tokens
{
    std::vector<std::string_view> items;
    std::vector<size_t> offsets;
};

Tokens tokenize(std::string_view line)
{
    Tokens tokens;
    size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && is_blank(line[i]))
        {
            ++i;
        }
        if (i >= line.size())
        {
            break;
        }
        const size_t begin = i;
        while (i < line.size() && !is_blank(line[i]))
        {
            ++i;
        }
        tokens.items.push_back(line.substr(begin, i - begin));
        tokens.offsets.push_back(begin);
    }
    return tokens;
}

std::string_view trim(std::string_view text)
{
    size_t begin = 0;
    while (begin < text.size() && (is_blank(text[begin]) || text[begin] == '\r'))
    {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && (is_blank(text[end - 1]) || text[end - 1] == '\r'))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

template <typename T>
bool parse_uint(std::string_view text, T& out, int base = 10)
{
    if (text.empty())
    {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, out, base);
    return ec == std::errc {} && ptr == end;
}

bool parse_double(std::string_view text, double& out)
{
    // from_chars for floating point is not available everywhere libstdc++ and
    // libc++ overlap, so this goes through strtod on a NUL-terminated copy.
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || errno == ERANGE)
    {
        return false;
    }
    out = value;
    return true;
}

// "1059.900" -> 1059900 microseconds. The fraction's meaning comes from how
// many digits it has: v1.1 writes one digit meaning tenths of a millisecond,
// everything later writes three meaning microseconds. Scaling by the digit
// count rather than by the version means a file that disagrees with its own
// header still reads correctly.
bool parse_offset_us(std::string_view text, uint64_t& out)
{
    const size_t dot = text.find('.');
    const std::string_view wholeText = text.substr(0, dot);

    uint64_t whole = 0;
    if (!parse_uint(wholeText, whole))
    {
        return false;
    }
    if (whole > std::numeric_limits<uint64_t>::max() / 1000u)
    {
        return false;
    }

    uint64_t micros = whole * 1000u;
    if (dot != std::string_view::npos)
    {
        const std::string_view fraction = text.substr(dot + 1);
        if (fraction.empty())
        {
            return false;
        }
        uint64_t scale = 1000;
        for (const char c : fraction)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
            if (scale == 0)
            {
                // More precision than a microsecond. Digits past the third are
                // dropped rather than rejected: the value is still valid, it is
                // just finer than this field can hold.
                continue;
            }
            scale /= 10;
            micros += static_cast<uint64_t>(c - '0') * scale;
        }
    }
    out = micros;
    return true;
}

struct TypeInfo
{
    RecordKind kind { RecordKind::Data };
    bool isFD { false };
    bool isBRS { false };
    bool isESI { false };
    // v1.x folds direction into this column; v2.x has a separate one.
    bool carriesDirection { false };
    bool isTx { false };
};

// The T column of a v2.x file.
bool decode_type_v2(std::string_view text, TypeInfo& out)
{
    out = TypeInfo {};
    if (text == "DT")
    {
        out.kind = RecordKind::Data;
    }
    else if (text == "FD")
    {
        out.kind = RecordKind::Data;
        out.isFD = true;
    }
    else if (text == "FB")
    {
        out.kind = RecordKind::Data;
        out.isFD = true;
        out.isBRS = true;
    }
    else if (text == "FE")
    {
        out.kind = RecordKind::Data;
        out.isFD = true;
        out.isESI = true;
    }
    else if (text == "BI")
    {
        out.kind = RecordKind::Data;
        out.isFD = true;
        out.isBRS = true;
        out.isESI = true;
    }
    else if (text == "RR")
    {
        out.kind = RecordKind::Remote;
    }
    else if (text == "ER")
    {
        out.kind = RecordKind::ErrorFrame;
    }
    else if (text == "ST")
    {
        out.kind = RecordKind::HardwareStatus;
    }
    else if (text == "EC")
    {
        out.kind = RecordKind::ErrorCounter;
    }
    else if (text == "EV")
    {
        out.kind = RecordKind::Event;
    }
    else if (text == "XL" || text == "PE" || text == "OF" || text == "EN")
    {
        // v3.0 records with no representation here.
        out.kind = RecordKind::Unsupported;
    }
    else
    {
        return false;
    }
    return true;
}

// The combined type/direction column of a v1.1+ file.
bool decode_type_v1(std::string_view text, TypeInfo& out)
{
    out = TypeInfo {};
    out.carriesDirection = true;
    if (text == "Rx")
    {
        out.kind = RecordKind::Data;
        out.isTx = false;
    }
    else if (text == "Tx")
    {
        out.kind = RecordKind::Data;
        out.isTx = true;
    }
    else if (text == "Warng")
    {
        out.kind = RecordKind::HardwareStatus;
    }
    else if (text == "Error")
    {
        out.kind = RecordKind::ErrorFrame;
    }
    else
    {
        return false;
    }
    return true;
}

// Whether a record of this kind carries an identifier token at all.
//
// This is the difference between v2.0 and v2.1 that breaks naive parsers. v2.0
// says the ID column "is empty" for EC, ER and ST -- and means it literally:
// the token is not there and every later column shifts left. v2.1 keeps the
// column and puts a '-' in it.
bool has_id_token(Version version, RecordKind kind)
{
    switch (kind)
    {
        case RecordKind::Data:
        case RecordKind::Remote:
        case RecordKind::Event:
        case RecordKind::Unsupported:
            return true;
        case RecordKind::ErrorFrame:
        case RecordKind::HardwareStatus:
        case RecordKind::ErrorCounter:
            return version != Version::V2_0;
    }
    return true;
}

// Same story for the length column.
bool has_length_token(Version version, RecordKind kind)
{
    switch (kind)
    {
        case RecordKind::Data:
        case RecordKind::Remote:
        case RecordKind::Event:
        case RecordKind::Unsupported:
            return true;
        case RecordKind::ErrorFrame:
        case RecordKind::HardwareStatus:
        case RecordKind::ErrorCounter:
            return version != Version::V2_0;
    }
    return true;
}

char column_letter(ColumnId id)
{
    switch (id)
    {
        case ColumnId::Number:    return 'N';
        case ColumnId::Offset:    return 'O';
        case ColumnId::Type:      return 'T';
        case ColumnId::Bus:       return 'B';
        case ColumnId::Id:        return 'I';
        case ColumnId::Direction: return 'd';
        case ColumnId::Reserved:  return 'R';
        case ColumnId::Length:    return 'l';
        case ColumnId::Dlc:       return 'L';
        case ColumnId::Data:      return 'D';
        case ColumnId::Vcid:      return 'V';
        case ColumnId::Sdt:       return 'S';
        case ColumnId::Af:        return 'A';
        case ColumnId::Rrs:       return 'r';
        case ColumnId::Sec:       return 's';
    }
    return '?';
}

std::optional<ColumnId> column_from_letter(char c)
{
    switch (c)
    {
        case 'N': return ColumnId::Number;
        case 'O': return ColumnId::Offset;
        case 'T': return ColumnId::Type;
        case 'B': return ColumnId::Bus;
        case 'I': return ColumnId::Id;
        case 'd': return ColumnId::Direction;
        case 'R': return ColumnId::Reserved;
        case 'l': return ColumnId::Length;
        case 'L': return ColumnId::Dlc;
        case 'D': return ColumnId::Data;
        case 'V': return ColumnId::Vcid;
        case 'S': return ColumnId::Sdt;
        case 'A': return ColumnId::Af;
        case 'r': return ColumnId::Rrs;
        case 's': return ColumnId::Sec;
        default:  return std::nullopt;
    }
}

// How many hex digits an 11-bit identifier is written with. v3.0 dropped it
// from four to three, which is the only thing in that version that changes how
// an otherwise-readable line parses.
size_t standard_id_width(Version version)
{
    return version == Version::V3_0 ? 3u : 4u;
}

// Whether the file's 'L' column is a CAN FD length code. Only v2.0 and later
// have one; v1.x calls its column a DLC but writes a byte count in it.
bool dlc_is_length_code(Version version)
{
    switch (version)
    {
        case Version::V1_0:
        case Version::V1_1:
        case Version::V1_2:
        case Version::V1_3:
            return false;
        case Version::V2_0:
        case Version::V2_1:
        case Version::V3_0:
            return true;
    }
    return true;
}

} // namespace

const char* to_string(Version version)
{
    switch (version)
    {
        case Version::V1_0: return "1.0";
        case Version::V1_1: return "1.1";
        case Version::V1_2: return "1.2";
        case Version::V1_3: return "1.3";
        case Version::V2_0: return "2.0";
        case Version::V2_1: return "2.1";
        case Version::V3_0: return "3.0";
    }
    return "?";
}

std::string version_text(Version version)
{
    return to_string(version);
}

Result<Version> parse_version(std::string_view text)
{
    const std::string_view trimmed = trim(text);
    if (trimmed == "1.0") { return Version::V1_0; }
    if (trimmed == "1.1") { return Version::V1_1; }
    if (trimmed == "1.2") { return Version::V1_2; }
    if (trimmed == "1.3") { return Version::V1_3; }
    if (trimmed == "2.0") { return Version::V2_0; }
    if (trimmed == "2.1") { return Version::V2_1; }
    if (trimmed == "3.0") { return Version::V3_0; }
    return invalid_argument(
        fmt::format("'{}' is not a TRC file version this build knows; expected one of "
                    "1.0, 1.1, 1.2, 1.3, 2.0, 2.1, 3.0",
                    trimmed));
}

const char* to_string(RecordKind kind)
{
    switch (kind)
    {
        case RecordKind::Data:           return "data";
        case RecordKind::Remote:         return "remote request";
        case RecordKind::ErrorFrame:     return "error frame";
        case RecordKind::HardwareStatus: return "hardware status";
        case RecordKind::ErrorCounter:   return "error counter";
        case RecordKind::Event:          return "event";
        case RecordKind::Unsupported:    return "unsupported";
    }
    return "?";
}

bool Columns::has(ColumnId id) const
{
    return std::find(order.begin(), order.end(), id) != order.end();
}

Columns default_columns(Version version)
{
    Columns columns;
    switch (version)
    {
        case Version::V1_0:
            // No type column at all: a v1.0 record's kind comes from its ID
            // being FFFFFFFF or its data starting with ERROR or RTR.
            columns.order = { ColumnId::Number, ColumnId::Offset, ColumnId::Id,
                              ColumnId::Dlc, ColumnId::Data };
            break;
        case Version::V1_1:
            columns.order = { ColumnId::Number, ColumnId::Offset, ColumnId::Type,
                              ColumnId::Id, ColumnId::Dlc, ColumnId::Data };
            break;
        case Version::V1_2:
            columns.order = { ColumnId::Number, ColumnId::Offset, ColumnId::Bus,
                              ColumnId::Type, ColumnId::Id, ColumnId::Dlc, ColumnId::Data };
            break;
        case Version::V1_3:
            columns.order = { ColumnId::Number, ColumnId::Offset, ColumnId::Bus,
                              ColumnId::Type, ColumnId::Id, ColumnId::Reserved,
                              ColumnId::Dlc, ColumnId::Data };
            break;
        case Version::V2_0:
            columns.order = { ColumnId::Number, ColumnId::Offset, ColumnId::Type,
                              ColumnId::Id, ColumnId::Direction, ColumnId::Length,
                              ColumnId::Data };
            break;
        case Version::V2_1:
        case Version::V3_0:
            columns.order = { ColumnId::Number, ColumnId::Offset, ColumnId::Type,
                              ColumnId::Bus, ColumnId::Id, ColumnId::Direction,
                              ColumnId::Reserved, ColumnId::Dlc, ColumnId::Data };
            break;
    }
    return columns;
}

Result<Columns> parse_columns(std::string_view text)
{
    Columns columns;
    size_t begin = 0;
    while (begin <= text.size())
    {
        const size_t comma = text.find(',', begin);
        const std::string_view field
            = trim(text.substr(begin, comma == std::string_view::npos ? std::string_view::npos
                                                                      : comma - begin));
        if (field.size() != 1)
        {
            return invalid_argument(fmt::format(
                "'{}' is not a TRC column identifier; each is a single case-sensitive letter",
                field));
        }
        const auto id = column_from_letter(field[0]);
        if (!id.has_value())
        {
            return invalid_argument(
                fmt::format("'{}' is not a TRC column identifier", field[0]));
        }
        if (columns.has(*id))
        {
            return invalid_argument(
                fmt::format("column '{}' is listed twice", field[0]));
        }
        columns.order.push_back(*id);

        if (comma == std::string_view::npos)
        {
            break;
        }
        begin = comma + 1;
    }

    if (columns.order.empty())
    {
        return invalid_argument("$COLUMNS lists no columns");
    }
    if (columns.has(ColumnId::Length) && columns.has(ColumnId::Dlc))
    {
        return invalid_argument(
            "$COLUMNS has both 'l' and 'L'; a file declares a byte count or a length code, "
            "not both");
    }
    if (!columns.has(ColumnId::Offset) || !columns.has(ColumnId::Type))
    {
        return invalid_argument("$COLUMNS must contain at least O and T");
    }
    return columns;
}

double unix_us_to_ole_date(uint64_t unixUs)
{
    return (static_cast<double>(unixUs) / 1e6 / kSecondsPerDay) + kOleUnixEpochDays;
}

uint64_t ole_date_to_unix_us(double oleDate)
{
    const double seconds = (oleDate - kOleUnixEpochDays) * kSecondsPerDay;
    if (seconds <= 0.0)
    {
        return 0;
    }
    return static_cast<uint64_t>(std::llround(seconds * 1e6));
}

// --- Reader -----------------------------------------------------------------

Reader::~Reader() = default;

Result<std::unique_ptr<Reader>> Reader::open(const std::string& path)
{
    auto reader = std::unique_ptr<Reader>(new Reader());
    reader->file_.open(path, std::ios::binary);
    if (!reader->file_.is_open())
    {
        return not_found(fmt::format("cannot open trace '{}'", path));
    }
    reader->fromString_ = false;
    reader->header_.columns = default_columns(reader->header_.version);
    return reader;
}

std::unique_ptr<Reader> Reader::from_string(std::string text)
{
    auto reader = std::unique_ptr<Reader>(new Reader());
    reader->text_ = std::move(text);
    reader->fromString_ = true;
    reader->header_.columns = default_columns(reader->header_.version);
    return reader;
}

bool Reader::read_line(std::string& out)
{
    if (fromString_)
    {
        if (textPos_ >= text_.size())
        {
            return false;
        }
        const size_t newline = text_.find('\n', textPos_);
        if (newline == std::string::npos)
        {
            out.assign(text_, textPos_, text_.size() - textPos_);
            textPos_ = text_.size();
        }
        else
        {
            out.assign(text_, textPos_, newline - textPos_);
            textPos_ = newline + 1;
        }
        return true;
    }

    if (!std::getline(file_, out))
    {
        return false;
    }
    return true;
}

void Reader::apply_header_line(std::string_view line)
{
    // Everything here is a comment; only the $-keywords carry meaning.
    const std::string_view body = trim(line.substr(1));
    if (body.empty() || body[0] != '$')
    {
        constexpr std::string_view kGeneratedBy = "Generated by ";
        if (body.starts_with(kGeneratedBy))
        {
            header_.generatedBy = std::string(trim(body.substr(kGeneratedBy.size())));
        }
        return;
    }

    const size_t equals = body.find('=');
    if (equals == std::string_view::npos)
    {
        return;
    }
    const std::string_view keyword = body.substr(0, equals);
    // PCAN-Explorer 5 wrote a stray ';' after the $STARTTIME value; strip any
    // trailing punctuation rather than rejecting a file over it.
    std::string_view value = trim(body.substr(equals + 1));
    while (!value.empty() && (value.back() == ';' || value.back() == '\r'))
    {
        value.remove_suffix(1);
    }

    if (keyword == "$FILEVERSION")
    {
        auto version = parse_version(value);
        if (!version.has_value())
        {
            SPDLOG_WARN("trc: {}; reading it as {}", version.error().message,
                        to_string(header_.version));
            return;
        }
        header_.version = *version;
        if (!columnsDeclared_)
        {
            header_.columns = default_columns(header_.version);
        }
    }
    else if (keyword == "$STARTTIME")
    {
        double oleDate = 0.0;
        if (!parse_double(value, oleDate))
        {
            SPDLOG_WARN("trc: '{}' is not a $STARTTIME value", value);
            return;
        }
        header_.startTimeUnixUs = ole_date_to_unix_us(oleDate);
    }
    else if (keyword == "$COLUMNS")
    {
        auto columns = parse_columns(value);
        if (!columns.has_value())
        {
            SPDLOG_WARN("trc: {}; using the default layout for {}", columns.error().message,
                        to_string(header_.version));
            return;
        }
        header_.columns = std::move(*columns);
        columnsDeclared_ = true;
    }
}

Result<Record> Reader::parse_line(std::string_view line) const
{
    const Tokens tokens = tokenize(line);
    if (tokens.items.empty())
    {
        return invalid_argument("blank line");
    }

    Record record;
    record.line = stats_.lines;

    const Version version = header_.version;
    const bool typeColumnIsV1 = version == Version::V1_1 || version == Version::V1_2
        || version == Version::V1_3;

    TypeInfo type;
    // v1.0 has no type column; the kind is worked out from the ID and the data
    // once both have been seen.
    bool typeKnown = version != Version::V1_0;

    size_t ti = 0;
    size_t declaredLength = 0;
    bool lengthSeen = false;
    bool idIsPlaceholder = false;
    std::string_view idToken;

    auto need = [&](ColumnId column) -> Result<std::string_view> {
        if (ti >= tokens.items.size())
        {
            return invalid_argument(fmt::format(
                "line ends before the '{}' column; it has {} tokens and the layout needs more",
                column_letter(column), tokens.items.size()));
        }
        return tokens.items[ti++];
    };

    for (const ColumnId column : header_.columns.order)
    {
        switch (column)
        {
            case ColumnId::Number:
            {
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                // v1.x writes "12)"; v2.x writes "12".
                std::string_view text = *token;
                if (!text.empty() && text.back() == ')')
                {
                    text.remove_suffix(1);
                }
                if (!parse_uint(text, record.number))
                {
                    return invalid_argument(
                        fmt::format("'{}' is not a message number", *token));
                }
                break;
            }

            case ColumnId::Offset:
            {
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                if (!parse_offset_us(*token, record.offsetUs))
                {
                    return invalid_argument(
                        fmt::format("'{}' is not a time offset", *token));
                }
                break;
            }

            case ColumnId::Type:
            {
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                const bool ok = typeColumnIsV1 ? decode_type_v1(*token, type)
                                               : decode_type_v2(*token, type);
                if (!ok)
                {
                    return invalid_argument(
                        fmt::format("'{}' is not a record type in a {} file", *token,
                                    to_string(version)));
                }
                typeKnown = true;
                record.kind = type.kind;
                if (type.carriesDirection)
                {
                    record.isTx = type.isTx;
                }
                break;
            }

            case ColumnId::Bus:
            {
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                if (*token == "-")
                {
                    record.bus = 0;
                }
                else if (!parse_uint(*token, record.bus) || record.bus < 1 || record.bus > 16)
                {
                    return invalid_argument(
                        fmt::format("'{}' is not a bus number; the format allows 1 to 16, or "
                                    "'-' for a record tied to no bus",
                                    *token));
                }

                // An EV record's text starts right after the bus and runs to
                // the end of the line, so no further column applies.
                if (record.kind == RecordKind::Event)
                {
                    if (ti < tokens.items.size())
                    {
                        record.event = std::string(trim(line.substr(tokens.offsets[ti])));
                    }
                    return record;
                }
                break;
            }

            case ColumnId::Id:
            {
                if (typeKnown && !has_id_token(version, record.kind))
                {
                    break;
                }
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                idToken = *token;
                if (idToken == "-")
                {
                    idIsPlaceholder = true;
                    break;
                }
                for (const char c : idToken)
                {
                    if (!is_hex_digit(c))
                    {
                        return invalid_argument(
                            fmt::format("'{}' is not a hexadecimal identifier", idToken));
                    }
                }
                uint32_t id = 0;
                if (!parse_uint(idToken, id, 16))
                {
                    return invalid_argument(
                        fmt::format("'{}' does not fit a 32-bit identifier", idToken));
                }
                record.frame.id = id;

                // The token's *width* is what says 11-bit or 29-bit. An
                // eight-digit 00000123 and a four-digit 0123 are different
                // messages on the same bus, and this is the only thing in the
                // file that distinguishes them.
                const size_t standardWidth = standard_id_width(version);
                if (idToken.size() == 8)
                {
                    record.frame.isExtended = true;
                }
                else if (idToken.size() <= standardWidth)
                {
                    record.frame.isExtended = false;
                }
                else
                {
                    return invalid_argument(fmt::format(
                        "'{}' is {} hex digits; a {} file writes {} for an 11-bit identifier "
                        "and 8 for a 29-bit one",
                        idToken, idToken.size(), to_string(version), standardWidth));
                }
                break;
            }

            case ColumnId::Direction:
            {
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                if (*token == "Rx")
                {
                    record.isTx = false;
                }
                else if (*token == "Tx")
                {
                    record.isTx = true;
                }
                else
                {
                    return invalid_argument(
                        fmt::format("'{}' is not a direction; expected Rx or Tx", *token));
                }
                break;
            }

            case ColumnId::Reserved:
            {
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                if (*token != "-")
                {
                    uint8_t address = 0;
                    if (!parse_uint(*token, address))
                    {
                        return invalid_argument(fmt::format(
                            "'{}' is not a J1939 destination address, and is not '-'", *token));
                    }
                    record.destinationAddress = address;
                }
                break;
            }

            case ColumnId::Length:
            case ColumnId::Dlc:
            {
                if (typeKnown && !has_length_token(version, record.kind))
                {
                    break;
                }
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                uint16_t raw = 0;
                if (!parse_uint(*token, raw))
                {
                    return invalid_argument(
                        fmt::format("'{}' is not a data length", *token));
                }
                if (column == ColumnId::Dlc && dlc_is_length_code(version))
                {
                    // A v2.x 'L' column is a CAN FD length *code*: 15 means 64
                    // bytes, not 15. can::dlc_to_length owns that table.
                    if (raw > 15)
                    {
                        return invalid_argument(fmt::format(
                            "'{}' is not a CAN FD length code; the field is four bits", *token));
                    }
                    declaredLength
                        = can::dlc_to_length(static_cast<uint8_t>(raw), type.isFD);
                }
                else
                {
                    // v1.x calls the column a DLC, but for classic CAN a DLC of
                    // 0..8 and a byte count are the same number, and v1.3
                    // widened it to a real J1939 length.
                    declaredLength = raw;
                }
                lengthSeen = true;
                break;
            }

            case ColumnId::Data:
            {
                // v1.x marks a remote request and a v1.0 error frame with a
                // word where the payload would be.
                if (ti < tokens.items.size())
                {
                    if (tokens.items[ti] == "RTR")
                    {
                        record.kind = RecordKind::Remote;
                        typeKnown = true;
                        ++ti;
                        break;
                    }
                    if (tokens.items[ti] == "ERROR")
                    {
                        record.kind = RecordKind::ErrorFrame;
                        typeKnown = true;
                        ++ti;
                    }
                }

                // Collect two-digit hex bytes and stop at the first token that
                // is not one. That is what ends the payload of a v1.x error
                // warning, whose line finishes with '--' padding and the short
                // names of the flags that were set.
                size_t count = 0;
                while (ti < tokens.items.size())
                {
                    const std::string_view token = tokens.items[ti];
                    if (token.size() != 2 || !is_hex_digit(token[0]) || !is_hex_digit(token[1]))
                    {
                        break;
                    }
                    uint8_t byte = 0;
                    if (!parse_uint(token, byte, 16))
                    {
                        break;
                    }
                    if (count >= kMaxPayload)
                    {
                        return invalid_argument(fmt::format(
                            "more than {} payload bytes; this build carries CAN and CAN FD "
                            "frames, not J1939 large messages or CAN XL",
                            kMaxPayload));
                    }
                    record.frame.data[count] = byte;
                    ++count;
                    ++ti;
                }
                record.frame.len = static_cast<uint8_t>(count);
                break;
            }

            case ColumnId::Vcid:
            case ColumnId::Sdt:
            case ColumnId::Af:
            case ColumnId::Rrs:
            case ColumnId::Sec:
            {
                // v3.0 CAN XL columns. They are consumed so the layout stays
                // aligned for the records that are readable; the records that
                // actually use them are reported as unsupported.
                auto token = need(column);
                if (!token.has_value()) { return std::unexpected(token.error()); }
                break;
            }
        }
    }

    // v1.0 has no type column, so the kind falls out of what was read: the
    // format reserves the identifier FFFFFFFF for an error warning.
    if (!typeKnown)
    {
        if (idToken == "FFFFFFFF")
        {
            record.kind = RecordKind::HardwareStatus;
        }
        else
        {
            record.kind = RecordKind::Data;
        }
    }
    else if (record.kind == RecordKind::HardwareStatus && idToken == "FFFFFFFF")
    {
        // v1.1+ says the same thing twice. The type column already decided.
    }

    // The identifier only has to be a legal one when it is addressing a device.
    // A hardware-status record parks FFFFFFFF there, and an error frame's
    // identifier is a bitmap of what went wrong.
    switch (record.kind)
    {
        case RecordKind::Data:
        case RecordKind::Remote:
            if (!idIsPlaceholder && !record.frame.id_fits())
            {
                return invalid_argument(fmt::format(
                    "identifier 0x{:X} does not fit the {}-bit form its {} hex digits declare",
                    record.frame.id, record.frame.isExtended ? 29 : 11, idToken.size()));
            }
            break;
        case RecordKind::ErrorFrame:
        case RecordKind::HardwareStatus:
        case RecordKind::ErrorCounter:
        case RecordKind::Event:
        case RecordKind::Unsupported:
            break;
    }

    // The declared length and the payload actually present have to agree. The
    // old parser took the smaller of the two, so a truncated line produced a
    // short frame that looked exactly like a real one.
    switch (record.kind)
    {
        case RecordKind::Data:
            record.frame.isFD = type.isFD;
            record.frame.isBRS = type.isBRS;
            record.frame.isESI = type.isESI;
            if (lengthSeen && declaredLength != record.frame.len)
            {
                return invalid_argument(fmt::format(
                    "the length column says {} bytes and the line carries {}", declaredLength,
                    record.frame.len));
            }
            break;

        case RecordKind::Remote:
            record.frame.isRTR = true;
            if (record.frame.len != 0)
            {
                return invalid_argument(fmt::format(
                    "a remote request carries no payload, and this line has {} bytes",
                    record.frame.len));
            }
            // A remote request's length is the length it is *asking* for, which
            // is what `len` means on an RTR frame everywhere else in this tree
            // and in SocketCAN. Dropping it to zero would lose the only thing
            // the record says.
            if (lengthSeen)
            {
                if (declaredLength > kMaxPayload)
                {
                    return invalid_argument(fmt::format(
                        "a remote request asking for {} bytes does not fit a CAN frame",
                        declaredLength));
                }
                record.frame.len = static_cast<uint8_t>(declaredLength);
            }
            break;

        case RecordKind::ErrorFrame:
        case RecordKind::HardwareStatus:
        case RecordKind::ErrorCounter:
            record.frame.isError = true;
            if (lengthSeen && declaredLength != record.frame.len)
            {
                return invalid_argument(fmt::format(
                    "the length column says {} bytes and the line carries {}", declaredLength,
                    record.frame.len));
            }
            break;

        case RecordKind::Event:
        case RecordKind::Unsupported:
            break;
    }

    if (header_.startTimeUnixUs != 0)
    {
        record.frame.timestampUs = header_.startTimeUnixUs + record.offsetUs;
    }

    return record;
}

Result<std::optional<Record>> Reader::next()
{
    while (read_line(lineBuffer_))
    {
        ++stats_.lines;

        const std::string_view line = trim(lineBuffer_);
        if (line.empty())
        {
            continue;
        }
        if (line[0] == ';')
        {
            apply_header_line(line);
            continue;
        }

        auto record = parse_line(line);
        if (!record.has_value())
        {
            ++stats_.badLines;
            if (warningsLogged_ < kMaxWarnings)
            {
                ++warningsLogged_;
                SPDLOG_WARN("trc: line {} skipped: {}", stats_.lines, record.error().message);
                if (warningsLogged_ == kMaxWarnings)
                {
                    SPDLOG_WARN("trc: further bad lines will not be logged; the count is in "
                                "ReadStats::badLines");
                }
            }
            continue;
        }

        if (record->kind == RecordKind::Unsupported)
        {
            ++stats_.unsupported;
            continue;
        }

        ++stats_.records;
        return std::optional<Record> { std::move(*record) };
    }

    if (!fromString_ && file_.bad())
    {
        return io_error("reading the trace failed");
    }
    return std::optional<Record> {};
}

// --- Writer -----------------------------------------------------------------

namespace
{

// "10/11/2025 16:36:10.195.0", the shape PCAN-View writes in its comment block.
std::string format_start_comment(uint64_t unixUs)
{
    const auto seconds = static_cast<std::time_t>(unixUs / 1000000u);
    const auto micros = static_cast<unsigned>(unixUs % 1000000u);
    std::tm parts {};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    return fmt::format("{:02}/{:02}/{:04} {:02}:{:02}:{:02}.{:03}.{}", parts.tm_mon + 1,
                       parts.tm_mday, parts.tm_year + 1900, parts.tm_hour, parts.tm_min,
                       parts.tm_sec, micros / 1000u, (micros % 1000u) / 100u);
}

const char* type_text(const Record& record)
{
    switch (record.kind)
    {
        case RecordKind::Data:
            if (!record.frame.isFD)
            {
                return "DT";
            }
            if (record.frame.isBRS && record.frame.isESI) { return "BI"; }
            if (record.frame.isBRS)                       { return "FB"; }
            if (record.frame.isESI)                       { return "FE"; }
            return "FD";
        case RecordKind::Remote:         return "RR";
        case RecordKind::ErrorFrame:     return "ER";
        case RecordKind::HardwareStatus: return "ST";
        case RecordKind::ErrorCounter:   return "EC";
        case RecordKind::Event:          return "EV";
        case RecordKind::Unsupported:    return "??";
    }
    return "??";
}

} // namespace

Writer::~Writer()
{
    if (file_.is_open())
    {
        file_.flush();
    }
}

Result<std::unique_ptr<Writer>> Writer::create(const std::string& path,
                                               const WriterOptions& options)
{
    if (options.version != Version::V2_1)
    {
        return unsupported(fmt::format(
            "this build writes TRC 2.1, not {}. 2.1 is the last version before CAN XL and the "
            "only one carrying bus, direction and the FD flags at once",
            to_string(options.version)));
    }

    auto writer = std::unique_ptr<Writer>(new Writer());
    writer->file_.open(path, std::ios::binary | std::ios::trunc);
    if (!writer->file_.is_open())
    {
        return io_error(fmt::format("cannot create trace '{}'", path));
    }
    writer->options_ = options;
    if (writer->options_.startTimeUnixUs == 0)
    {
        writer->options_.startTimeUnixUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    auto written = writer->write_header();
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return writer;
}

Result<void> Writer::write_header()
{
    // The three $-keywords are the only lines a reader is obliged to
    // understand, so they are written exactly as the specification gives them.
    // Everything below is the comment block PCAN's own tools emit, kept because
    // a human opening the file expects to see it.
    file_ << ";$FILEVERSION=" << version_text(options_.version) << "\r\n";
    file_ << fmt::format(";$STARTTIME={:.10f}\r\n",
                         unix_us_to_ole_date(options_.startTimeUnixUs));
    file_ << ";$COLUMNS=N,O,T,B,I,d,R,l,D\r\n";
    file_ << ";\r\n";
    file_ << ";   Start time: " << format_start_comment(options_.startTimeUnixUs) << "\r\n";
    file_ << ";   Generated by " << options_.generatedBy << "\r\n";

    if (!options_.buses.empty())
    {
        file_ << ";-----------------------------------------------------------------------"
                 "--------\r\n";
        file_ << ";   Bus  Name                  Connection            Bit rate\r\n";
        for (const BusInfo& bus : options_.buses)
        {
            std::string rate;
            if (bus.bitrateBps != 0)
            {
                rate = fmt::format("Nominal {} kbit/s", bus.bitrateBps / 1000u);
                if (bus.dataBitrateBps != 0)
                {
                    rate += fmt::format(", Data {} kbit/s", bus.dataBitrateBps / 1000u);
                }
            }
            file_ << fmt::format(";   {:<4} {:<21} {:<21} {}\r\n", bus.bus, bus.name,
                                 bus.connection, rate);
        }
    }

    file_ << ";---------------------------------------------------------------------------"
             "----\r\n";
    file_ << ";   Message   Time    Type    ID      Rx/Tx\r\n";
    file_ << ";   Number    Offset | Bus [hex] | Reserved\r\n";
    file_ << ";   |         [ms]    | |     |       | | Data Length\r\n";
    file_ << ";   |         |       | |     |       | | |      Data [hex] ...\r\n";
    file_ << ";   |         |       | |     |       | | |      |\r\n";
    file_ << ";---+-- ------+------ +- +- --+----- +- +- +--- +- -- -- -- -- -- -- --\r\n";

    headerWritten_ = true;
    if (!file_)
    {
        return io_error("writing the trace header failed");
    }
    return {};
}

Result<void> Writer::write(const Record& record)
{
    if (!headerWritten_)
    {
        return invalid_state("the trace header has not been written");
    }
    if (record.kind == RecordKind::Unsupported)
    {
        return invalid_argument("an unsupported record has no TRC 2.1 spelling");
    }

    const std::string busText = record.bus == 0 ? std::string("-")
                                                : fmt::format("{}", record.bus);

    if (record.kind == RecordKind::Event)
    {
        // "EV. Event. User-defined text, begins directly after bus specifier."
        file_ << fmt::format("{:>7} {:>13} EV {} {}\r\n", record.number,
                             fmt::format("{}.{:03}", record.offsetUs / 1000u,
                                         record.offsetUs % 1000u),
                             busText, record.event);
        ++recordsWritten_;
        return file_ ? Result<void> {} : io_error("writing a trace record failed");
    }

    // The identifier's width is load-bearing: it is what tells a reader whether
    // this was an 11-bit or a 29-bit frame.
    std::string idText;
    switch (record.kind)
    {
        case RecordKind::Data:
        case RecordKind::Remote:
            idText = record.frame.isExtended ? fmt::format("{:08X}", record.frame.id)
                                             : fmt::format("{:04X}", record.frame.id);
            break;
        case RecordKind::ErrorFrame:
        case RecordKind::HardwareStatus:
        case RecordKind::ErrorCounter:
        case RecordKind::Event:
        case RecordKind::Unsupported:
            idText = "-";
            break;
    }

    const std::string reservedText
        = record.destinationAddress.has_value()
        ? fmt::format("{}", *record.destinationAddress)
        : std::string("-");

    // A remote request declares the length it is *asking* for and writes no
    // data of its own, so its `len` is a promise about the reply rather than a
    // count of bytes to emit. Writing those bytes produces a line that reads
    // back as a data frame.
    const size_t payload
        = record.kind == RecordKind::Remote ? 0u : std::min<size_t>(record.frame.len, kMaxPayload);
    const size_t declared = std::min<size_t>(record.frame.len, kMaxPayload);

    scratch_.clear();
    for (size_t i = 0; i < payload; ++i)
    {
        if (i != 0)
        {
            scratch_ += ' ';
        }
        scratch_ += fmt::format("{:02X}", record.frame.data[i]);
    }

    file_ << fmt::format("{:>7} {:>13} {} {:>2} {:>8} {} {} {:<4} {}\r\n", record.number,
                         fmt::format("{}.{:03}", record.offsetUs / 1000u,
                                     record.offsetUs % 1000u),
                         type_text(record), busText, idText, record.isTx ? "Tx" : "Rx",
                         reservedText, declared, scratch_);

    ++recordsWritten_;
    if (!file_)
    {
        return io_error("writing a trace record failed");
    }
    return {};
}

Result<void> Writer::flush()
{
    file_.flush();
    if (!file_)
    {
        return io_error("flushing the trace failed");
    }
    return {};
}

} // namespace can::trc
