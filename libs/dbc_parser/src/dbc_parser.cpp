#include "dbc_parser/dbc_parser.h"

#include <charconv>
#include <cstdlib>
#include <regex>
#include <string>

#include <spdlog/spdlog.h>

namespace dbc_parser
{

static bool parseUint(const std::string &s, uint32_t &out)
{
    auto first = s.data();
    auto last = s.data() + s.size();
    unsigned long long temp = 0ULL;
    auto [ptr, ec] = std::from_chars(first, last, temp);
    if (ec != std::errc() || ptr != last)
    {
        return false;
    }

    out = static_cast<uint32_t>(temp);
    return true;
}

static bool parseDouble(const std::string &s, double &out)
{
    char *end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

Parser::Parser(std::string_view input)
{
    Lexer lex(input);
    tokens_ = lex.tokenize();
}

const Token &Parser::peek() const
{
    return tokens_[index_];
}

const Token &Parser::get()
{
    return tokens_[index_++];
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

bool Parser::expect(TokenKind kind, ParseError &err, std::string_view what) {
    if (accept(kind))
    {
        return true;
    }
    err.line = peek().line;
    err.column = peek().column;
    err.message = std::string("Expected ") + std::string(what);
    SPDLOG_ERROR("{} (line {})", err.message,peek().line);
    return false;
}

std::optional<Database> Parser::parse(ParseError &errorOut)
{
    Database db;
    // Single forward pass in known section order
    while (!eof())
    {
        if (peek().kind != TokenKind::Identifier)
        {
            get();
            continue;
        }

        const Token &tok = peek();

        if (tok.lexeme == "VERSION")
        {
            if (!parseVersion(db, errorOut))
            {
                SPDLOG_ERROR("Failed to parse VERSION ({}, line {})", errorOut.message, errorOut.line);
            }

            continue;
        }
        if (tok.lexeme == "NS_")
        {
            if (!parseNamespaceSection(db, errorOut))
            {
                SPDLOG_ERROR("Failed to parse NS_ ({}, line {})", errorOut.message, errorOut.line);
            }

            continue;
        }
        if (tok.lexeme == "BS_") // skip bit timing section lines
        {
            get();

            while (!eof() && peek().kind != TokenKind::Newline)
            {
                get();
            }

            if (peek().kind == TokenKind::Newline)
            {
                get();
            }

            continue;
        }
        if (tok.lexeme == "BU_")
        {
            if (!parseNodes(db, errorOut))
            {
                SPDLOG_ERROR("Failed to parse BU_ ({}, line {})", errorOut.message, errorOut.line);
            }
            continue;
        }
        if (tok.lexeme == "BO_")
        {
            if (!parseMessage(db, errorOut))
            {
                SPDLOG_ERROR("Failed to parse BO_ ({}, line {})", errorOut.message, errorOut.line);
            }
            continue;
        }
        if (tok.lexeme == "CM_")
        {
            if (!parseComment(db, errorOut))
            {
                SPDLOG_ERROR("Failed to parse CM_ ({}, line {})", errorOut.message, errorOut.line);
            }
            continue;
        }
        if (tok.lexeme == "VAL_")
        {
            if (!parseValueTable(db, errorOut))
            {
                SPDLOG_ERROR("Failed to parse VAL_ ({}, line {})", errorOut.message, errorOut.line);
            }
            continue;
        }

        // Fallback: skip line
        while (!eof() && peek().kind != TokenKind::Newline)
        {
            get();
        }
    }

    return db;
}

bool Parser::parseVersion(Database &db, ParseError &err)
{
    // VERSION "..."
    get(); // VERSION
    if (peek().kind == TokenKind::String)
    {
        db.version = get().lexeme;
        // consume to end of line
        while (!eof() && peek().kind != TokenKind::Newline)
        {
            get();
        }
        if (peek().kind == TokenKind::Newline)
        {
            get();
        }
        return true;
    }
    // Some DBCs have VERSION without quotes. Accept identifier/number fallback.
    if (peek().kind == TokenKind::Identifier || peek().kind == TokenKind::Number)
    {
        db.version = get().lexeme;
        while (!eof() && peek().kind != TokenKind::Newline)
        {
            get();
        }
        if (peek().kind == TokenKind::Newline)
        {
            get();
        }
        return true;
    }
    err.line = peek().line;
    err.column = peek().column;
    err.message = "VERSION expects a string or identifier";
    return false;
}

bool Parser::parseNodes(Database &db, ParseError & /*err*/)
{
    // BU_: nodeA nodeB ... (may span after identifier)
    get(); // BU_
    if (!accept(TokenKind::Colon))
    {
        // Some files put BU_: on same tokenization as identifier then colon as separate token, which we handle
    }
    // Collect until newline
    while (!eof() && peek().kind != TokenKind::Newline)
    {
        if (peek().kind == TokenKind::Identifier)
        {
            db.nodes.push_back(get().lexeme);
        }
        else
        {
            get();
        }
    }
    if (peek().kind == TokenKind::Newline)
    {
        get();
    }

    return true;
}

bool Parser::parseMessage(Database &db, ParseError &err)
{
    // BO_ <id> <name> : <dlc> <transmitter>
    get(); // BO_
    if (peek().kind != TokenKind::Number)
    {
        err.message = "BO_ expects numeric id";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    Message msg;
    uint32_t id = 0;
    if (!parseUint(get().lexeme, id))
    {
        err.message = "Invalid message id";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    msg.id = id;

    if (peek().kind != TokenKind::Identifier)
    {
        err.message = "BO_ expects name";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }
    msg.name = get().lexeme;

    if (!accept(TokenKind::Colon))
    {
        err.message = "BO_ expects ':'";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    if (peek().kind != TokenKind::Number)
    {
        err.message = "BO_ expects DLC";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    uint32_t dlc = 0;

    if (!parseUint(get().lexeme, dlc))
    {
        err.message = "Invalid DLC";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    msg.dlc = dlc;

    if (peek().kind != TokenKind::Identifier)
    {
        err.message = "BO_ expects transmitter";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    msg.transmitter = get().lexeme;

    // End of message header line
    while (!eof() && peek().kind != TokenKind::Newline)
    {
        get();
    }

    if (peek().kind == TokenKind::Newline)
    {
        get();
    }

    // Read following SG_ lines until next top-level section (anything not SG_)
    while (!eof())
    {
        if (peek().kind == TokenKind::Newline)
        {
            get();
            continue;
        }
        if (peek().kind == TokenKind::Identifier)
        {
            if (peek().lexeme == "SG_")
            {
                if (!parseSignal(msg, err)) return false;
                // attach signal comments if any (legacy; currently unused)
                if (auto it = signalCommentsByMsgId_.find(msg.id); it != signalCommentsByMsgId_.end())
                {
                    auto &byName = it->second;
                    if (!msg.signals.empty())
                    {
                        Signal &last = msg.signals.back();
                        if (auto it2 = byName.find(last.name); it2 != byName.end())
                        {
                            last.comment = it2->second;
                        }
                    }
                }
                continue;
            }
            // Any other identifier indicates end of this message's signal section.
            break;
        }
        // consume unrecognized line within message
        break;
    }
    db.messages.push_back(std::move(msg));
    return true;
}

bool Parser::parseSignal(Message &msg, ParseError &err)
{
    // SG_ <name> : <start>|<len>@<endianness><sign> (<scale>,<offset>) [<min>|<max>] "unit" <receivers...>
    get(); // SG_
    if (peek().kind != TokenKind::Identifier)
    {
        err.message = "SG_ expects name";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    Signal sig;
    sig.name = get().lexeme;
    // Optional multiplexer token between name and ':' (e.g., m4, m0, or M)
    if (peek().kind == TokenKind::Identifier)
    {
        const std::string &maybeMux = peek().lexeme;
        bool isMuxToken = false;
        bool isMuxorToken = false;
        uint32_t muxGroup = 0;

        if (maybeMux == "M")
        {
            isMuxToken = true;
            isMuxorToken = true;
        }
        else if (!maybeMux.empty() && maybeMux[0] == 'm')
        {
            // m followed by digits
            isMuxToken = maybeMux.size() > 1;
            for (size_t i = 1; isMuxToken && i < maybeMux.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(maybeMux[i])))
                {
                    isMuxToken = false;
                }
            }
            if (isMuxToken)
            {
                // parse digits after 'm'
                const std::string digits = maybeMux.substr(1);
                uint32_t val = 0;
                if (parseUint(digits, val))
                {
                    muxGroup = val;
                }
            }
        }
        if (isMuxToken)
        {
            get(); // consume mux token
            sig.isMultiplexor = isMuxorToken;
            sig.isMultiplex = !isMuxorToken;
            if (!isMuxorToken)
            {
                sig.multiplexedGroupIdx = muxGroup;
            }
            msg.isMultiplexed = true;
        }
    }

    if (!accept(TokenKind::Colon))
    {
        err.message = "SG_ expects ':'";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    // start
    if (peek().kind != TokenKind::Number)
    {
        err.message = "SG_ expects start bit";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    uint32_t start = 0;
    if (!parseUint(get().lexeme, start))
    {
        err.message = "Invalid start bit";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    sig.startBit = start;
    if (!accept(TokenKind::Pipe))
    {
        err.message = "SG_ expects '|'";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    // len
    if (peek().kind != TokenKind::Number)
    {
        err.message = "SG_ expects length";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }
    
    uint32_t len = 0;
    if (!parseUint(get().lexeme, len))
    {
        err.message = "Invalid length";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    sig.length = len;
    if (!accept(TokenKind::At))
    {
        err.message = "SG_ expects '@'";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    // endianness and sign marker like 0+ or 1-
    if (peek().kind != TokenKind::Number)
    {
        err.message = "SG_ expects endianness marker";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    uint32_t endMarker = 0;

    if (!parseUint(get().lexeme, endMarker))
    {
        err.message = "Invalid endianness marker";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    // DBC: @0 = Motorola (big-endian), @1 = Intel (little-endian)
    sig.littleEndian = (endMarker == 1);

    if (peek().kind == TokenKind::Plus)
    {
        sig.isSigned = false; get();
    }
    else if (peek().kind == TokenKind::Minus)
    {
        sig.isSigned = true;
        get();
    }

    // (scale,offset)
    if (!accept(TokenKind::LParen))
    {
        err.message = "SG_ expects '('";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    if (peek().kind != TokenKind::Number)
    {
        err.message = "SG_ expects scale number";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    double scale = 0.0;
    if (!parseDouble(get().lexeme, scale))
    {
        err.message = "Invalid scale";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    sig.scale = scale;
    accept(TokenKind::Comma);
    double offset = 0.0;
    if (peek().kind != TokenKind::Number)
    {
        err.message = "SG_ expects offset number";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    if (!parseDouble(get().lexeme, offset))
    {
        err.message = "Invalid offset";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }

    sig.offset = offset;

    if (!accept(TokenKind::RParen))
    {
        /* tolerate missing ) */
    }

    // [min|max]
    if (accept(TokenKind::LBracket))
    {
        double minv = 0.0, maxv = 0.0;
        if (peek().kind == TokenKind::Number)
        {
            parseDouble(get().lexeme, minv);
        }

        // TODO: fix this
        if (!accept(TokenKind::Pipe)) {}
    
        if (peek().kind == TokenKind::Number)
        {
            parseDouble(get().lexeme, maxv);
        }
        accept(TokenKind::RBracket);
        sig.minimum = minv; sig.maximum = maxv;
    }

    // "unit"
    if (peek().kind == TokenKind::String)
    {
        sig.unit = get().lexeme;
    }

    // Receivers list until end of line, identifiers separated by commas
    while (!eof() && peek().kind != TokenKind::Newline)
    {
        if (peek().kind == TokenKind::Identifier)
        {
            sig.receivers.push_back(get().lexeme);
        }
        else
        {
            get();
        }
    }

    if (peek().kind == TokenKind::Newline)
    {
        get();
    }

    msg.signals.push_back(std::move(sig));
    return true;
}

bool Parser::parseComment(Database &db, ParseError &err)
{
    // CM_ BO_ <id> "..." or CM_ SG_ <id> <sig> "..."
    get(); // CM_

    if (peek().kind != TokenKind::Identifier)
    {
        while (!eof() && peek().kind != TokenKind::Newline)
        {
            get();
        }
        if (peek().kind == TokenKind::Newline)
        {
            get();
        }

        return true;
    }

    std::string kind = get().lexeme; // BO_ or SG_
    if (kind == "BO_")
    {
        if (peek().kind != TokenKind::Number)
        {
            err.message = "CM_ BO_ expects id";
            err.line = peek().line;
            err.column = peek().column;
            return false;
        }

        uint32_t id = 0;
        if (!parseUint(get().lexeme, id))
        {
            err.message = "Invalid id";
            err.line = peek().line;
            err.column = peek().column;
            return false;
        }

        // DBC terminates with ';' after string; tolerate missing semicolon too
        if (peek().kind == TokenKind::String)
        {
            std::string c = get().lexeme;
            // Remove linefeeds from the comment.
            c.erase(std::remove(c.begin(), c.end(), '\n'), c.end());
            // Remove carriage return characters (\r)
            c.erase(std::remove(c.begin(), c.end(), '\r'), c.end());

            if (peek().kind == TokenKind::Semicolon)
            {
                get();
            }

            for (auto &m : db.messages)
            {
                if (m.id == id)
                {
                    m.comment = std::move(c);
                    break;
                }
            }
        }
    }
    else if (kind == "SG_")
    {
        if (peek().kind != TokenKind::Number)
        {
            err.message = "CM_ SG_ expects message id";
            err.line = peek().line;
            err.column = peek().column;
            return false;
        }

        uint32_t id = 0;
        if (!parseUint(get().lexeme, id))
        {
            err.message = "Invalid id";
            err.line = peek().line;
            err.column = peek().column;
            return false;
        }

        if (peek().kind != TokenKind::Identifier)
        {
            err.message = "CM_ SG_ expects signal name";
            err.line = peek().line;
            err.column = peek().column;
            return false;
        }

        std::string sigName = get().lexeme;

        if (peek().kind == TokenKind::String)
        {
            std::string c = get().lexeme;
            // Remove linefeeds from the comment.
            c.erase(std::remove(c.begin(), c.end(), '\n'), c.end());
            // Remove carriage return characters (\r)
            c.erase(std::remove(c.begin(), c.end(), '\r'), c.end());

            if (peek().kind == TokenKind::Semicolon)
            {
                get();
            }

            for (auto &m : db.messages)
            {
                if (m.id == id)
                {
                    for (auto &s : m.signals)
                    {
                        if (s.name == sigName)
                        {
                            s.comment = std::move(c);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }

    while (!eof() && peek().kind != TokenKind::Newline)
    {
        get();
    }

    if (peek().kind == TokenKind::Newline)
    {
        get();
    }

    return true;
}

bool Parser::parseValueTable(Database &db, ParseError &err)
{
    // Format: VAL_ <msgId> <signalName> <raw> "text" <raw> "text" ... ;
    get(); // VAL_
    if (peek().kind != TokenKind::Number)
    {
        err.message = "VAL_ expects message id";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }
    uint32_t msgId = 0;
    if (!parseUint(get().lexeme, msgId))
    {
        err.message = "Invalid VAL_ message id";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }
    if (peek().kind != TokenKind::Identifier)
    {
        err.message = "VAL_ expects signal name";
        err.line = peek().line;
        err.column = peek().column;
        return false;
    }
    std::string sigName = get().lexeme;

    // Parse pairs: number string ... until end of line or ';'
    std::vector<ValueMapping> mappings;
    while (!eof() && peek().kind != TokenKind::Newline)
    {
        if (peek().kind == TokenKind::Semicolon)
        {
            get();
            break;
        }
        if (peek().kind != TokenKind::Number)
        {
            // If stray tokens appear, consume to end of line
            while (!eof() && peek().kind != TokenKind::Newline) get();
            break;
        }
        int64_t raw = 0;
        {
            // allow negative raw values
            std::string num = get().lexeme;
            raw = std::strtoll(num.c_str(), nullptr, 10);
        }
        if (peek().kind != TokenKind::String)
        {
            // malformed pair; bail out of this VAL_ line
            while (!eof() && peek().kind != TokenKind::Newline) get();
            break;
        }
        std::string text = get().lexeme;
        // Replace any non-alphanumeric characters with underscores.
        text = std::regex_replace(text, std::regex("[^a-zA-Z0-9]"), "_");

        mappings.push_back(ValueMapping{raw, std::move(text)});
    }
    if (peek().kind == TokenKind::Newline)
    {
        get();
    }

    // Check to see if there are any duplicate enum values. If so, append the raw value to the description.
    for (auto &m : mappings)
    {
        for (auto &n : mappings)
        {
            if (m.description == n.description)
            {
                n.description += "_" + std::to_string(m.rawValue);
            }
        }
    }

    // Attach mappings to the corresponding signal
    for (auto &m : db.messages)
    {
        if (m.id == msgId)
        {
            for (auto &s : m.signals)
            {
                if (s.name == sigName)
                {
                    s.valueTable = std::move(mappings);
                    return true;
                }
            }
            break;
        }
    }
    return true;
}

bool Parser::parseNamespaceSection(Database & /*db*/, ParseError & /*err*/)
{
    // NS_ : followed by a list of identifiers possibly across lines until a blank line or another top-level token
    get(); // NS_
    // Optional colon
    if (accept(TokenKind::Colon))
    {
        // consume the rest of the current line
        while (!eof() && peek().kind != TokenKind::Newline)
        {
            get();
        }

        if (peek().kind == TokenKind::Newline)
        {
            get();
        }
    }

    // The section usually lists keywords each on its own line; skip until we hit a blank line or a known top-level marker
    while (!eof())
    {
        // Stop if next token starts a new well-known section
        if (peek().kind == TokenKind::Identifier)
        {
            const std::string &kw = peek().lexeme;
            if (kw == "BU_" || kw == "BO_" || kw == "VERSION" || kw == "BS_")
            {
                break;
            }
        }

        // advance to next line
        while (!eof() && peek().kind != TokenKind::Newline)
        {
            get();
        }

        if (peek().kind == TokenKind::Newline)
        {
            get();
        }

        // A blank line usually ends NS_ block as well
        if (peek().kind == TokenKind::Newline)
        {
            get();
            break; 
        }
    }
    return true;
}

} // namespace dbc_parser


