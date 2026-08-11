#include "dbc_parser/dbc_parser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace dbc_parser
{
namespace
{

// These conversions have to ignore the global locale. strtod does not: under a
// comma-decimal locale it stops at the '.' in "0.1", which used to fail the
// scale parse and -- because failures were swallowed -- delete the enclosing
// message from the build. std::from_chars is the integer answer; see
// parseDoubleText for why the floating point one cannot be.
bool parseUintText(const std::string &text, uint32_t &out)
{
    uint64_t wide = 0;
    const char *first = text.data();
    const char *last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, wide);
    if ((ec != std::errc()) || (ptr != last) || (wide > 0xFFFFFFFFull))
    {
        return false;
    }
    out = static_cast<uint32_t>(wide);
    return true;
}

bool parseInt64Text(const std::string &text, int64_t &out)
{
    const char *first = text.data();
    const char *last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return (ec == std::errc()) && (ptr == last);
}

bool parseDoubleText(const std::string &text, double &out)
{
    // NOT std::from_chars, unlike the integer parses above. Apple's libc++
    // implements the floating point overloads in the dylib rather than the
    // header and marks them "introduced in macOS 26.0", so building against an
    // older deployment target -- which this project derives from whatever host
    // it is built on -- has no symbol to call and does not compile.
    //
    // A stream imbued with the classic locale is the portable way to keep the
    // locale independence that matters here: num_get takes the decimal point
    // from the imbued locale, not from whatever the process locale has become.
    // The parse is strict for the same reason from_chars was -- failbit for a
    // malformed or out-of-range value, eof only if every character was
    // consumed. It is slower, which does not matter: this runs once per numeric
    // token while reading a DBC, not per CAN frame.
    std::istringstream stream{text};
    stream.imbue(std::locale::classic());

    double value = 0.0;
    stream >> value;
    if (stream.fail() || !stream.eof())
    {
        return false;
    }
    out = value;

    // A non-finite scale or offset would be emitted into generated source as
    // `inf`, which is not valid C++.
    return std::isfinite(out);
}

constexpr std::array kReservedWords{
    std::string_view{"alignas"},      std::string_view{"alignof"},
    std::string_view{"and"},          std::string_view{"and_eq"},
    std::string_view{"asm"},          std::string_view{"auto"},
    std::string_view{"bitand"},       std::string_view{"bitor"},
    std::string_view{"bool"},         std::string_view{"break"},
    std::string_view{"case"},         std::string_view{"catch"},
    std::string_view{"char"},         std::string_view{"char16_t"},
    std::string_view{"char32_t"},     std::string_view{"char8_t"},
    std::string_view{"class"},        std::string_view{"co_await"},
    std::string_view{"co_return"},    std::string_view{"co_yield"},
    std::string_view{"compl"},        std::string_view{"concept"},
    std::string_view{"const"},        std::string_view{"const_cast"},
    std::string_view{"consteval"},    std::string_view{"constexpr"},
    std::string_view{"constinit"},    std::string_view{"continue"},
    std::string_view{"decltype"},     std::string_view{"default"},
    std::string_view{"delete"},       std::string_view{"do"},
    std::string_view{"double"},       std::string_view{"dynamic_cast"},
    std::string_view{"else"},         std::string_view{"enum"},
    std::string_view{"explicit"},     std::string_view{"export"},
    std::string_view{"extern"},       std::string_view{"false"},
    std::string_view{"float"},        std::string_view{"for"},
    std::string_view{"friend"},       std::string_view{"goto"},
    std::string_view{"if"},           std::string_view{"inline"},
    std::string_view{"int"},          std::string_view{"long"},
    std::string_view{"mutable"},      std::string_view{"namespace"},
    std::string_view{"new"},          std::string_view{"noexcept"},
    std::string_view{"not"},          std::string_view{"not_eq"},
    std::string_view{"nullptr"},      std::string_view{"operator"},
    std::string_view{"or"},           std::string_view{"or_eq"},
    std::string_view{"private"},      std::string_view{"protected"},
    std::string_view{"public"},       std::string_view{"register"},
    std::string_view{"reinterpret_cast"}, std::string_view{"requires"},
    std::string_view{"return"},       std::string_view{"short"},
    std::string_view{"signed"},       std::string_view{"sizeof"},
    std::string_view{"static"},       std::string_view{"static_assert"},
    std::string_view{"static_cast"},  std::string_view{"struct"},
    std::string_view{"switch"},       std::string_view{"template"},
    std::string_view{"this"},         std::string_view{"thread_local"},
    std::string_view{"throw"},        std::string_view{"true"},
    std::string_view{"try"},          std::string_view{"typedef"},
    std::string_view{"typeid"},       std::string_view{"typename"},
    std::string_view{"union"},        std::string_view{"unsigned"},
    std::string_view{"using"},        std::string_view{"virtual"},
    std::string_view{"void"},         std::string_view{"volatile"},
    std::string_view{"wchar_t"},      std::string_view{"while"},
    std::string_view{"xor"},          std::string_view{"xor_eq"},
};

// Sections we recognise but model nothing from. Skipping these is a deliberate
// decision rather than a hole, so they do not deserve a warning; anything not
// on this list and not handled does.
constexpr std::array kIgnoredSections{
    std::string_view{"BO_TX_BU_"},     std::string_view{"SIG_GROUP_"},
    std::string_view{"EV_"},           std::string_view{"EV_DATA_"},
    std::string_view{"ENVVAR_DATA_"},  std::string_view{"CAT_"},
    std::string_view{"CAT_DEF_"},      std::string_view{"FILTER"},
    std::string_view{"SGTYPE_"},       std::string_view{"SGTYPE_VAL_"},
    std::string_view{"BA_SGTYPE_"},    std::string_view{"SIG_TYPE_REF_"},
    std::string_view{"SIGTYPE_VALTYPE_"}, std::string_view{"NS_DESC_"},
};

bool contains(const auto &haystack, std::string_view needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

std::string hex(uint32_t value)
{
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    do
    {
        out.insert(out.begin(), kDigits[value & 0xFu]);
        value >>= 4u;
    } while (value != 0u);
    return out;
}

} // namespace

bool isUsableIdentifier(std::string_view name)
{
    if (name.empty())
    {
        return false;
    }

    if (!std::isalpha(static_cast<unsigned char>(name.front())) && (name.front() != '_'))
    {
        return false;
    }

    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && (c != '_'))
        {
            return false;
        }
    }

    return !contains(kReservedWords, name);
}

Parser::Parser(std::string_view input)
{
    Lexer lex(input, diags_);
    tokens_ = lex.tokenize();
}

const Diagnostics &Parser::diagnostics() const
{
    return diags_;
}

const Token &Parser::peek() const
{
    return peekAhead(0);
}

const Token &Parser::peekAhead(size_t offset) const
{
    // tokenize() always appends EndOfFile, so clamping to the last token means
    // lookahead past the end reads that rather than running off the vector.
    size_t wanted = index_ + offset;
    if (wanted >= tokens_.size())
    {
        wanted = tokens_.size() - 1;
    }
    return tokens_[wanted];
}

const Token &Parser::get()
{
    const Token &tok = peek();
    if (index_ < (tokens_.size() - 1))
    {
        index_ += 1;
    }
    return tok;
}

bool Parser::eof() const
{
    return peek().kind == TokenKind::EndOfFile;
}

bool Parser::accept(TokenKind kind)
{
    if (peek().kind == kind)
    {
        get();
        return true;
    }
    return false;
}

bool Parser::expect(TokenKind kind, std::string_view context)
{
    if (accept(kind))
    {
        return true;
    }

    errorHere(std::string(context) + " expects " + std::string(describe(kind)) +
              ", found " + std::string(describe(peek().kind)));
    return false;
}

void Parser::errorHere(std::string message)
{
    diags_.error(peek().line, peek().column, std::move(message));
}

void Parser::warnHere(std::string message)
{
    diags_.warn(peek().line, peek().column, std::move(message));
}

void Parser::skipToNextLine()
{
    while (!eof() && (peek().kind != TokenKind::Newline))
    {
        get();
    }
    accept(TokenKind::Newline);
}

bool Parser::takeUint(uint32_t &out, std::string_view what)
{
    if (peek().kind != TokenKind::Number)
    {
        errorHere(std::string(what) + " expects a number, found " +
                  std::string(describe(peek().kind)));
        return false;
    }

    const Token &tok = get();
    if (!parseUintText(tok.lexeme, out))
    {
        diags_.error(tok.line, tok.column,
                     std::string(what) + ": '" + tok.lexeme +
                         "' is not a whole number that fits in 32 bits");
        return false;
    }
    return true;
}

bool Parser::takeInt64(int64_t &out, std::string_view what)
{
    if (peek().kind != TokenKind::Number)
    {
        errorHere(std::string(what) + " expects a number, found " +
                  std::string(describe(peek().kind)));
        return false;
    }

    const Token &tok = get();
    if (!parseInt64Text(tok.lexeme, out))
    {
        diags_.error(tok.line, tok.column,
                     std::string(what) + ": '" + tok.lexeme +
                         "' is not a whole number that fits in 64 bits");
        return false;
    }
    return true;
}

bool Parser::takeDouble(double &out, std::string_view what)
{
    if (peek().kind != TokenKind::Number)
    {
        errorHere(std::string(what) + " expects a number, found " +
                  std::string(describe(peek().kind)));
        return false;
    }

    const Token &tok = get();
    if (!parseDoubleText(tok.lexeme, out))
    {
        diags_.error(tok.line, tok.column,
                     std::string(what) + ": '" + tok.lexeme + "' is not a finite number");
        return false;
    }
    return true;
}

bool Parser::takeIdentifier(std::string &out, std::string_view what)
{
    if (peek().kind != TokenKind::Identifier)
    {
        errorHere(std::string(what) + " expects a name, found " +
                  std::string(describe(peek().kind)));
        return false;
    }
    out = get().lexeme;
    return true;
}

bool Parser::takeString(std::string &out, std::string_view what)
{
    if (peek().kind != TokenKind::String)
    {
        errorHere(std::string(what) + " expects a quoted string, found " +
                  std::string(describe(peek().kind)));
        return false;
    }
    out = get().lexeme;
    return true;
}

std::optional<Database> Parser::parse()
{
    Database db;
    std::set<std::string> warnedKeywords;

    while (!eof())
    {
        if (peek().kind == TokenKind::Newline)
        {
            get();
            continue;
        }

        if (peek().kind != TokenKind::Identifier)
        {
            errorHere("expected a section keyword, found " +
                      std::string(describe(peek().kind)));
            skipToNextLine();
            continue;
        }

        const std::string keyword = peek().lexeme;

        if (keyword == "VERSION")
        {
            parseVersion(db);
        }
        else if (keyword == "NS_")
        {
            parseSkippedSection(keyword);
        }
        else if (keyword == "BS_")
        {
            get();
            skipToNextLine();
        }
        else if (keyword == "BU_")
        {
            parseNodes(db);
        }
        else if (keyword == "BO_")
        {
            parseMessage(db);
        }
        else if (keyword == "SG_")
        {
            errorHere("SG_ outside of a BO_ message");
            skipToNextLine();
        }
        else if (keyword == "CM_")
        {
            parseComment();
        }
        else if (keyword == "VAL_")
        {
            parseValueTable(db);
        }
        else if (keyword == "VAL_TABLE_")
        {
            parseNamedValueTable(db);
        }
        else if (keyword == "SIG_VALTYPE_")
        {
            parseSignalValueType();
        }
        else if (keyword == "BA_")
        {
            parseAttribute();
        }
        else if ((keyword == "BA_DEF_") || (keyword == "BA_DEF_DEF_") ||
                 (keyword == "BA_DEF_REL_") || (keyword == "BA_DEF_DEF_REL_") ||
                 (keyword == "BA_REL_") || (keyword == "BA_DEF_SGTYPE_"))
        {
            parseAttributeDefinition();
        }
        else if (keyword == "SG_MUL_VAL_")
        {
            // Extended multiplexing changes which signals are valid for a given
            // multiplexor value. Ignoring it does not lose a field, it decodes
            // the wrong ones -- so this has to stop the build, not warn.
            errorHere("SG_MUL_VAL_ (extended multiplexing) is not supported, and "
                      "ignoring it would decode the wrong signals");
            skipToNextLine();
        }
        else if (contains(kIgnoredSections, keyword))
        {
            get();
            skipToNextLine();
        }
        else
        {
            if (warnedKeywords.insert(keyword).second)
            {
                warnHere("ignoring unrecognised section '" + keyword + "'");
            }
            skipToNextLine();
        }
    }

    resolvePending(db);
    validate(db);

    if (diags_.hasErrors())
    {
        return std::nullopt;
    }

    return db;
}

void Parser::parseVersion(Database &db)
{
    get(); // VERSION

    if (peek().kind == TokenKind::String)
    {
        db.version = get().lexeme;
    }
    else if ((peek().kind == TokenKind::Identifier) || (peek().kind == TokenKind::Number))
    {
        // Some tools emit VERSION without quotes.
        db.version = get().lexeme;
    }
    else if (peek().kind != TokenKind::Newline)
    {
        errorHere("VERSION expects a quoted string");
    }

    skipToNextLine();
}

void Parser::parseNodes(Database &db)
{
    get(); // BU_
    accept(TokenKind::Colon);

    while (!eof() && (peek().kind != TokenKind::Newline))
    {
        if (peek().kind == TokenKind::Identifier)
        {
            db.nodes.push_back(get().lexeme);
        }
        else
        {
            errorHere("BU_ expects node names, found " +
                      std::string(describe(peek().kind)));
            skipToNextLine();
            return;
        }
    }

    accept(TokenKind::Newline);
}

void Parser::parseMessage(Database &db)
{
    const int headerLine = peek().line;
    get(); // BO_

    Message msg;

    uint32_t rawId = 0;
    if (!takeUint(rawId, "BO_ message id"))
    {
        skipToNextLine();
        return;
    }

    // A DBC records an extended (29-bit) identifier by setting bit 31. A CAN
    // driver never sets that bit, so an id carried through verbatim would be
    // compared against incoming frames and match nothing at all.
    msg.isExtended = (rawId & 0x80000000u) != 0u;
    msg.id = rawId & 0x1FFFFFFFu;
    if (msg.id > 0x7FFu)
    {
        msg.isExtended = true;
    }

    if (!takeIdentifier(msg.name, "BO_ message name"))
    {
        skipToNextLine();
        return;
    }

    if (!expect(TokenKind::Colon, "BO_"))
    {
        skipToNextLine();
        return;
    }

    if (!takeUint(msg.dlc, "BO_ data length"))
    {
        skipToNextLine();
        return;
    }

    if (!takeIdentifier(msg.transmitter, "BO_ transmitter"))
    {
        skipToNextLine();
        return;
    }

    if (peek().kind != TokenKind::Newline)
    {
        errorHere("unexpected text after the BO_ header");
    }
    skipToNextLine();

    // Signals belong to this message until a line starts with something else.
    while (!eof())
    {
        if (peek().kind == TokenKind::Newline)
        {
            get();
            continue;
        }

        if ((peek().kind == TokenKind::Identifier) && (peek().lexeme == "SG_"))
        {
            if (!parseSignal(msg))
            {
                skipToNextLine();
            }
            continue;
        }

        break;
    }

    messageDefinitionLines_.emplace_back(msg.id, headerLine);
    db.messages.push_back(std::move(msg));
}

bool Parser::parseSignal(Message &msg)
{
    // SG_ <name> [mux] : <start>|<len>@<endian><sign> (<scale>,<offset>)
    //     [<min>|<max>] "unit" <receivers...>
    get(); // SG_

    Signal sig;
    if (!takeIdentifier(sig.name, "SG_ signal name"))
    {
        return false;
    }

    // Optional multiplexer token between the name and ':'. `M` selects,
    // `m<idx>` is gated, and `m<idx>M` is both -- the combined form used to
    // fall through and take the whole message down on the missing ':'.
    if (peek().kind == TokenKind::Identifier)
    {
        const std::string token = peek().lexeme;
        bool isMultiplexor = (token == "M");
        bool isMultiplexed = false;
        uint32_t group = 0;

        if (!isMultiplexor && (token.size() > 1) && (token.front() == 'm'))
        {
            std::string digits = token.substr(1);
            bool trailingM = (digits.back() == 'M');
            if (trailingM)
            {
                digits.pop_back();
            }

            const bool allDigits =
                !digits.empty() &&
                std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                });

            if (allDigits && parseUintText(digits, group))
            {
                isMultiplexed = true;
                isMultiplexor = trailingM;
            }
        }

        if (isMultiplexor || isMultiplexed)
        {
            get();
            sig.isMultiplexor = isMultiplexor;
            sig.isMultiplex = isMultiplexed;
            sig.multiplexedGroupIdx = group;
            msg.isMultiplexed = true;
        }
    }

    if (!expect(TokenKind::Colon, "SG_"))
    {
        return false;
    }

    if (!takeUint(sig.startBit, "SG_ start bit"))
    {
        return false;
    }

    if (!expect(TokenKind::Pipe, "SG_ bit layout"))
    {
        return false;
    }

    if (!takeUint(sig.length, "SG_ length"))
    {
        return false;
    }

    if (!expect(TokenKind::At, "SG_ bit layout"))
    {
        return false;
    }

    uint32_t endianMarker = 0;
    if (!takeUint(endianMarker, "SG_ byte order"))
    {
        return false;
    }

    if (endianMarker > 1u)
    {
        errorHere("SG_ byte order must be 0 (Motorola) or 1 (Intel)");
        return false;
    }

    sig.littleEndian = (endianMarker == 1u);

    if (accept(TokenKind::Plus))
    {
        sig.isSigned = false;
    }
    else if (accept(TokenKind::Minus))
    {
        sig.isSigned = true;
    }
    else
    {
        errorHere("SG_ expects '+' or '-' after the byte order");
        return false;
    }

    if (!expect(TokenKind::LParen, "SG_ scale and offset"))
    {
        return false;
    }

    if (!takeDouble(sig.scale, "SG_ scale"))
    {
        return false;
    }

    if (!expect(TokenKind::Comma, "SG_ scale and offset"))
    {
        return false;
    }

    if (!takeDouble(sig.offset, "SG_ offset"))
    {
        return false;
    }

    if (!expect(TokenKind::RParen, "SG_ scale and offset"))
    {
        return false;
    }

    if (!expect(TokenKind::LBracket, "SG_ range"))
    {
        return false;
    }

    if (!takeDouble(sig.minimum, "SG_ minimum"))
    {
        return false;
    }

    if (!expect(TokenKind::Pipe, "SG_ range"))
    {
        return false;
    }

    if (!takeDouble(sig.maximum, "SG_ maximum"))
    {
        return false;
    }

    if (!expect(TokenKind::RBracket, "SG_ range"))
    {
        return false;
    }

    if (!takeString(sig.unit, "SG_ unit"))
    {
        return false;
    }

    while (!eof() && (peek().kind != TokenKind::Newline))
    {
        if (peek().kind == TokenKind::Identifier)
        {
            sig.receivers.push_back(get().lexeme);
        }
        else if (peek().kind == TokenKind::Comma)
        {
            get();
        }
        else
        {
            errorHere("SG_ expects a comma separated receiver list, found " +
                      std::string(describe(peek().kind)));
            return false;
        }
    }

    accept(TokenKind::Newline);

    msg.signals.push_back(std::move(sig));
    return true;
}

void Parser::parseComment()
{
    const int line = peek().line;
    const int column = peek().column;
    get(); // CM_

    PendingComment pending;
    pending.line = line;
    pending.column = column;

    // A bare `CM_ "text";` is a comment on the database itself.
    if (peek().kind == TokenKind::String)
    {
        skipToNextLine();
        return;
    }

    if (!takeIdentifier(pending.kind, "CM_ target"))
    {
        skipToNextLine();
        return;
    }

    if (pending.kind == "BO_")
    {
        if (!takeUint(pending.messageId, "CM_ BO_ message id"))
        {
            skipToNextLine();
            return;
        }
    }
    else if (pending.kind == "SG_")
    {
        if (!takeUint(pending.messageId, "CM_ SG_ message id") ||
            !takeIdentifier(pending.signalName, "CM_ SG_ signal name"))
        {
            skipToNextLine();
            return;
        }
    }
    else
    {
        // CM_ BU_ / CM_ EV_ carry nothing the generated code exposes.
        skipToNextLine();
        return;
    }

    if (!takeString(pending.text, "CM_ text"))
    {
        skipToNextLine();
        return;
    }

    pending.messageId &= 0x1FFFFFFFu;
    accept(TokenKind::Semicolon);
    skipToNextLine();

    pendingComments_.push_back(std::move(pending));
}

bool Parser::parseValueMappings(std::vector<ValueMapping> &out)
{
    while (!eof() && (peek().kind != TokenKind::Newline))
    {
        if (accept(TokenKind::Semicolon))
        {
            return true;
        }

        int64_t raw = 0;
        if (!takeInt64(raw, "value table entry"))
        {
            return false;
        }

        std::string text;
        if (!takeString(text, "value table description"))
        {
            return false;
        }

        out.push_back(ValueMapping{raw, std::move(text)});
    }

    return true;
}

void Parser::parseValueTable(Database &db)
{
    (void)db;

    PendingValues pending;
    pending.line = peek().line;
    pending.column = peek().column;
    get(); // VAL_

    if (!takeUint(pending.messageId, "VAL_ message id") ||
        !takeIdentifier(pending.signalName, "VAL_ signal name"))
    {
        skipToNextLine();
        return;
    }

    pending.messageId &= 0x1FFFFFFFu;

    // `VAL_ <id> <signal> <TableName> ;` refers to a VAL_TABLE_ rather than
    // listing the pairs inline.
    if (peek().kind == TokenKind::Identifier)
    {
        pending.tableName = get().lexeme;
        accept(TokenKind::Semicolon);
        skipToNextLine();
        pendingValues_.push_back(std::move(pending));
        return;
    }

    if (!parseValueMappings(pending.mappings))
    {
        skipToNextLine();
        return;
    }

    skipToNextLine();
    pendingValues_.push_back(std::move(pending));
}

void Parser::parseNamedValueTable(Database &db)
{
    const int line = peek().line;
    const int column = peek().column;
    get(); // VAL_TABLE_

    std::string name;
    if (!takeIdentifier(name, "VAL_TABLE_ name"))
    {
        skipToNextLine();
        return;
    }

    std::vector<ValueMapping> mappings;
    if (!parseValueMappings(mappings))
    {
        skipToNextLine();
        return;
    }

    skipToNextLine();

    if (!db.valueTables.emplace(name, std::move(mappings)).second)
    {
        diags_.error(line, column, "VAL_TABLE_ '" + name + "' is defined more than once");
    }
}

void Parser::parseSignalValueType()
{
    PendingValueType pending;
    pending.line = peek().line;
    pending.column = peek().column;
    get(); // SIG_VALTYPE_

    if (!takeUint(pending.messageId, "SIG_VALTYPE_ message id") ||
        !takeIdentifier(pending.signalName, "SIG_VALTYPE_ signal name"))
    {
        skipToNextLine();
        return;
    }

    pending.messageId &= 0x1FFFFFFFu;
    accept(TokenKind::Colon);

    uint32_t code = 0;
    if (!takeUint(code, "SIG_VALTYPE_ type"))
    {
        skipToNextLine();
        return;
    }

    switch (code)
    {
    case 0u:
        pending.type = SignalValueType::Integer;
        break;

    case 1u:
        pending.type = SignalValueType::Float;
        break;

    case 2u:
        pending.type = SignalValueType::Double;
        break;

    default:
        errorHere("SIG_VALTYPE_ type must be 0 (integer), 1 (float) or 2 (double)");
        skipToNextLine();
        return;
    }

    accept(TokenKind::Semicolon);
    skipToNextLine();

    pendingValueTypes_.push_back(std::move(pending));
}

void Parser::parseAttribute()
{
    PendingAttribute pending;
    pending.line = peek().line;
    pending.column = peek().column;
    get(); // BA_

    if (!takeString(pending.name, "BA_ attribute name"))
    {
        skipToNextLine();
        return;
    }

    // Only the BO_-scoped form carries anything a message can use.
    if ((peek().kind != TokenKind::Identifier) || (peek().lexeme != "BO_"))
    {
        skipToNextLine();
        return;
    }

    get(); // BO_
    if (!takeUint(pending.messageId, "BA_ message id"))
    {
        skipToNextLine();
        return;
    }

    pending.messageId &= 0x1FFFFFFFu;

    if (peek().kind == TokenKind::String)
    {
        pending.value = get().lexeme;
    }
    else if (peek().kind == TokenKind::Number)
    {
        pending.value = get().lexeme;
    }
    else if (peek().kind == TokenKind::Identifier)
    {
        pending.value = get().lexeme;
    }

    accept(TokenKind::Semicolon);
    skipToNextLine();

    pendingAttributes_.push_back(std::move(pending));
}

void Parser::parseAttributeDefinition()
{
    // The definitions describe types and defaults for BA_ values. Nothing in
    // the generated code depends on them, but they are consumed here rather
    // than by the unknown-section fallback so a real typo still gets a warning.
    get();
    skipToNextLine();
}

void Parser::parseSkippedSection(std::string_view keyword)
{
    (void)keyword;

    get(); // NS_
    accept(TokenKind::Colon);
    skipToNextLine();

    // The NS_ body is one bare keyword per line. Testing for that shape rather
    // than for indentation means a file that does not indent the block still
    // ends up with the same sections parsed.
    while (!eof())
    {
        if (peek().kind == TokenKind::Newline)
        {
            get();
            continue;
        }

        const bool bareKeywordLine = (peek().kind == TokenKind::Identifier) &&
                                     (peekAhead(1).kind == TokenKind::Newline);
        if (!bareKeywordLine)
        {
            break;
        }

        get();
        get();
    }
}

Message *Parser::findMessage(Database &db, uint32_t id)
{
    for (auto &msg : db.messages)
    {
        if (msg.id == id)
        {
            return &msg;
        }
    }
    return nullptr;
}

void Parser::resolvePending(Database &db)
{
    for (const auto &pending : pendingComments_)
    {
        Message *msg = findMessage(db, pending.messageId);
        if (msg == nullptr)
        {
            diags_.warn(pending.line, pending.column,
                        "comment refers to message id " +
                            std::to_string(pending.messageId) + ", which is not defined");
            continue;
        }

        if (pending.kind == "BO_")
        {
            msg->comment = pending.text;
            continue;
        }

        auto found = std::find_if(msg->signals.begin(), msg->signals.end(),
                                  [&](const Signal &s) { return s.name == pending.signalName; });
        if (found == msg->signals.end())
        {
            diags_.warn(pending.line, pending.column,
                        "comment refers to signal '" + pending.signalName +
                            "', which message '" + msg->name + "' does not define");
            continue;
        }

        found->comment = pending.text;
    }

    for (auto &pending : pendingValues_)
    {
        Message *msg = findMessage(db, pending.messageId);
        if (msg == nullptr)
        {
            // Unlike a comment, a dropped value table silently changes the
            // generated type of a signal from an enum to a bare integer.
            diags_.error(pending.line, pending.column,
                         "VAL_ refers to message id " + std::to_string(pending.messageId) +
                             ", which is not defined");
            continue;
        }

        auto found = std::find_if(msg->signals.begin(), msg->signals.end(),
                                  [&](const Signal &s) { return s.name == pending.signalName; });
        if (found == msg->signals.end())
        {
            diags_.error(pending.line, pending.column,
                         "VAL_ refers to signal '" + pending.signalName +
                             "', which message '" + msg->name + "' does not define");
            continue;
        }

        if (!pending.tableName.empty())
        {
            auto table = db.valueTables.find(pending.tableName);
            if (table == db.valueTables.end())
            {
                diags_.error(pending.line, pending.column,
                             "VAL_ names value table '" + pending.tableName +
                                 "', which is not defined by any VAL_TABLE_");
                continue;
            }
            found->valueTable = table->second;
        }
        else
        {
            found->valueTable = std::move(pending.mappings);
        }
    }

    for (const auto &pending : pendingValueTypes_)
    {
        Message *msg = findMessage(db, pending.messageId);
        if (msg == nullptr)
        {
            diags_.error(pending.line, pending.column,
                         "SIG_VALTYPE_ refers to message id " +
                             std::to_string(pending.messageId) + ", which is not defined");
            continue;
        }

        auto found = std::find_if(msg->signals.begin(), msg->signals.end(),
                                  [&](const Signal &s) { return s.name == pending.signalName; });
        if (found == msg->signals.end())
        {
            diags_.error(pending.line, pending.column,
                         "SIG_VALTYPE_ refers to signal '" + pending.signalName +
                             "', which message '" + msg->name + "' does not define");
            continue;
        }

        found->valueType = pending.type;
    }

    for (const auto &pending : pendingAttributes_)
    {
        Message *msg = findMessage(db, pending.messageId);
        if (msg == nullptr)
        {
            continue;
        }

        msg->attributes[pending.name] = pending.value;

        // An explicit VFrameFormat overrides what the id's top bit implied.
        if (pending.name == "VFrameFormat")
        {
            if ((pending.value == "ExtendedCAN") || (pending.value == "J1939PG"))
            {
                msg->isExtended = true;
            }
            else if (pending.value == "StandardCAN")
            {
                msg->isExtended = false;
            }
        }
    }
}

void Parser::validate(Database &db)
{
    // A file with no BO_ in it is not a DBC we can do anything with, and
    // saying so here is what stops arbitrary text from generating a valid,
    // empty, useless library and exiting 0.
    if (db.messages.empty())
    {
        diags_.error(1, 1, "no BO_ message definitions found");
        return;
    }

    std::unordered_map<uint32_t, size_t> idCounts;
    std::unordered_set<std::string> seenNames;

    for (const auto &msg : db.messages)
    {
        idCounts[msg.id] += 1;
    }

    // messageDefinitionLines_ is appended in lockstep with db.messages, so the
    // index is the message's own entry rather than a lookup by id -- which
    // would pick the wrong one for exactly the duplicate ids reported here.
    for (size_t i = 0; i < db.messages.size(); ++i)
    {
        const Message &msg = db.messages[i];
        const int line = messageDefinitionLines_[i].second;

        if (idCounts[msg.id] > 1)
        {
            // Two messages with one id generate duplicate switch labels, and
            // only the first would ever be reached anyway.
            diags_.error(line, 1, "message id 0x" + hex(msg.id) +
                                      " is defined by more than one BO_");
            idCounts[msg.id] = 1; // report it once, not once per copy
        }

        if (!seenNames.insert(msg.name).second)
        {
            diags_.error(line, 1, "message name '" + msg.name + "' is used more than once");
        }

        validateMessage(msg, line);
    }
}

void Parser::validateMessage(const Message &msg, int line)
{
    if (!isUsableIdentifier(msg.name))
    {
        diags_.error(line, 1,
                     "message name '" + msg.name +
                         "' cannot be used as a C++ identifier in generated code");
    }

    // CAN is 8 bytes, CAN FD is up to 64. Anything past that is a typo, and
    // left alone it sizes a std::array the decoder then indexes out of.
    if (msg.dlc > 64u)
    {
        diags_.error(line, 1, "message '" + msg.name + "' declares a length of " +
                                  std::to_string(msg.dlc) + " bytes, which no CAN frame has");
    }

    size_t multiplexorCount = 0;
    for (const auto &sig : msg.signals)
    {
        if (sig.isMultiplexor)
        {
            multiplexorCount += 1;
        }
    }

    // `m3M` marks a signal that is gated by group 3 and itself selects a
    // nested level. The generated decoder has one flat multiplexor per
    // message, so it cannot express that -- and quietly treating the signal as
    // an ordinary multiplexor would decode the wrong fields, which is exactly
    // why SG_MUL_VAL_ is refused too. The token is still lexed properly, so
    // this reports the real problem instead of failing on a missing ':'.
    for (const auto &sig : msg.signals)
    {
        if (sig.isMultiplex && sig.isMultiplexor)
        {
            diags_.error(line, 1, "signal '" + msg.name + "." + sig.name +
                                      "' is both multiplexed and a multiplexor (nested "
                                      "multiplexing), which is not supported");
        }
    }

    if (msg.isMultiplexed && (multiplexorCount == 0))
    {
        // This used to reach the generator, which dereferenced the missing
        // multiplexor and died with a segfault part way through a header.
        diags_.error(line, 1,
                     "message '" + msg.name +
                         "' has multiplexed signals but no multiplexor signal (one "
                         "signal needs the 'M' marker)");
    }

    if (multiplexorCount > 1)
    {
        diags_.error(line, 1, "message '" + msg.name +
                                  "' has more than one multiplexor signal");
    }

    std::unordered_set<std::string> signalNames;
    for (const auto &sig : msg.signals)
    {
        if (!signalNames.insert(sig.name).second)
        {
            diags_.error(line, 1, "message '" + msg.name + "' defines signal '" +
                                      sig.name + "' more than once");
        }

        validateSignal(msg, sig, line);
    }
}

void Parser::validateSignal(const Message &msg, const Signal &sig, int line)
{
    const std::string where = "signal '" + msg.name + "." + sig.name + "'";

    if (!isUsableIdentifier(sig.name))
    {
        diags_.error(line, 1,
                     where + " cannot be used as a C++ identifier in generated code");
    }

    if (sig.length == 0u)
    {
        diags_.error(line, 1, where + " has zero length");
        return;
    }

    if (sig.length > 64u)
    {
        diags_.error(line, 1, where + " is " + std::to_string(sig.length) +
                                  " bits, and the decoder works in 64 bit words");
        return;
    }

    // Nothing used to check this, so a bad start bit produced generated code
    // that indexed past the end of the frame array with no warning anywhere.
    const uint32_t lastBit = sig.lastBitIndex();
    if (lastBit >= (msg.dlc * 8u))
    {
        diags_.error(line, 1, where + " occupies bit " + std::to_string(lastBit) +
                                  ", past the end of a " + std::to_string(msg.dlc) +
                                  " byte frame");
    }

    // encode() divides by the scale.
    if (sig.scale == 0.0)
    {
        diags_.error(line, 1, where + " has a scale of zero");
    }

    switch (sig.valueType)
    {
    case SignalValueType::Integer:
        break;

    case SignalValueType::Float:
        if (sig.length != 32u)
        {
            diags_.error(line, 1, where + " is declared IEEE float by SIG_VALTYPE_ but is " +
                                      std::to_string(sig.length) + " bits, not 32");
        }
        break;

    case SignalValueType::Double:
        if (sig.length != 64u)
        {
            diags_.error(line, 1, where + " is declared IEEE double by SIG_VALTYPE_ but is " +
                                      std::to_string(sig.length) + " bits, not 64");
        }
        break;
    }

    for (const auto &mapping : sig.valueTable)
    {
        const int64_t widest = (sig.length >= 64u)
                                   ? INT64_MAX
                                   : ((static_cast<int64_t>(1) << sig.length) - 1);
        if (!sig.isSigned && ((mapping.rawValue < 0) || (mapping.rawValue > widest)))
        {
            diags_.warn(line, 1, where + " has a value table entry " +
                                     std::to_string(mapping.rawValue) +
                                     " that the signal cannot represent");
        }
    }
}

} // namespace dbc_parser
