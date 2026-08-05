#include "dbc_parser/dbc_lexer.h"

#include <cctype>

namespace dbc_parser
{

std::string_view describe(TokenKind kind)
{
    switch (kind)
    {
    case TokenKind::EndOfFile:
        return "end of file";

    case TokenKind::Newline:
        return "end of line";

    case TokenKind::Identifier:
        return "a name";

    case TokenKind::Number:
        return "a number";

    case TokenKind::String:
        return "a quoted string";

    case TokenKind::Colon:
        return "':'";

    case TokenKind::Semicolon:
        return "';'";

    case TokenKind::At:
        return "'@'";

    case TokenKind::Plus:
        return "'+'";

    case TokenKind::Minus:
        return "'-'";

    case TokenKind::Pipe:
        return "'|'";

    case TokenKind::LParen:
        return "'('";

    case TokenKind::RParen:
        return "')'";

    case TokenKind::LBracket:
        return "'['";

    case TokenKind::RBracket:
        return "']'";

    case TokenKind::Comma:
        return "','";
    }

    return "something unrecognised";
}

Lexer::Lexer(std::string_view input, Diagnostics &diags) :
    input_(input),
    diags_(diags)
{
}

char Lexer::peek() const
{
    return peekAhead(0);
}

char Lexer::peekAhead(size_t offset) const
{
    if ((index_ + offset) >= input_.size())
    {
        return '\0';
    }
    return input_[index_ + offset];
}

char Lexer::get()
{
    if (index_ >= input_.size())
    {
        return '\0';
    }

    char c = input_[index_++];
    if (c == '\n')
    {
        line_ += 1;
        column_ = 1;
    }
    else
    {
        column_ += 1;
    }
    return c;
}

bool Lexer::eof() const
{
    return index_ >= input_.size();
}

void Lexer::skipWhitespaceExceptNewline()
{
    while (!eof())
    {
        char c = peek();
        if ((c == '\r') || (c == '\t') || (c == ' '))
        {
            get();
            continue;
        }

        // comments start with '//' skip to end of line
        if ((c == '/') && (peekAhead(1) == '/'))
        {
            while (!eof() && (peek() != '\n'))
            {
                get();
            }
            continue;
        }

        break;
    }
}

Token Lexer::readIdentifier()
{
    Token tok{TokenKind::Identifier, {}, line_, column_};
    while (!eof())
    {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || (c == '_') || (c == '.'))
        {
            tok.lexeme.push_back(get());
        }
        else
        {
            break;
        }
    }
    return tok;
}

Token Lexer::readNumber()
{
    Token tok{TokenKind::Number, {}, line_, column_};

    if (peek() == '-')
    {
        tok.lexeme.push_back(get());
    }

    while (!eof() && std::isdigit(static_cast<unsigned char>(peek())))
    {
        tok.lexeme.push_back(get());
    }

    if ((peek() == '.') && std::isdigit(static_cast<unsigned char>(peekAhead(1))))
    {
        tok.lexeme.push_back(get());
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek())))
        {
            tok.lexeme.push_back(get());
        }
    }

    // An exponent is only part of the number if it is actually well formed.
    // Swallowing a bare 'e' would turn `1eee` into a number token that no
    // conversion accepts, and the resulting "invalid scale" would point at the
    // wrong thing.
    if ((peek() == 'e') || (peek() == 'E'))
    {
        size_t digitOffset = 1;
        if ((peekAhead(1) == '+') || (peekAhead(1) == '-'))
        {
            digitOffset = 2;
        }

        if (std::isdigit(static_cast<unsigned char>(peekAhead(digitOffset))))
        {
            for (size_t i = 0; i < digitOffset; ++i)
            {
                tok.lexeme.push_back(get());
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek())))
            {
                tok.lexeme.push_back(get());
            }
        }
    }

    return tok;
}

Token Lexer::readString()
{
    Token tok{TokenKind::String, {}, line_, column_};

    // consume opening quote
    get();

    bool terminated = false;
    while (!eof())
    {
        char c = get();
        if (c == '"')
        {
            terminated = true;
            break;
        }

        // The lexeme holds the *unescaped* text. Anything emitting it into
        // generated source has to escape it again.
        if (c == '\\')
        {
            if (!eof())
            {
                tok.lexeme.push_back(get());
            }
        }
        else
        {
            tok.lexeme.push_back(c);
        }
    }

    if (!terminated)
    {
        diags_.error(tok.line, tok.column, "unterminated string");
    }

    return tok;
}

Token Lexer::punctuation(TokenKind kind)
{
    Token tok{kind, {}, line_, column_};
    tok.lexeme.push_back(get());
    return tok;
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    while (!eof())
    {
        // Normalize whitespace first (consumes '\r' in CRLF)
        skipWhitespaceExceptNewline();
        if (eof())
        {
            break;
        }

        // Now emit newline tokens on '\n'
        if (peek() == '\n')
        {
            tokens.push_back(punctuation(TokenKind::Newline));
            continue;
        }

        char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || (c == '_'))
        {
            tokens.push_back(readIdentifier());
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            tokens.push_back(readNumber());
            continue;
        }

        if ((c == '-') && std::isdigit(static_cast<unsigned char>(peekAhead(1))))
        {
            tokens.push_back(readNumber());
            continue;
        }

        switch (c)
        {
        case ':':
            tokens.push_back(punctuation(TokenKind::Colon));
            break;

        case ';':
            tokens.push_back(punctuation(TokenKind::Semicolon));
            break;

        case '@':
            tokens.push_back(punctuation(TokenKind::At));
            break;

        case '+':
            tokens.push_back(punctuation(TokenKind::Plus));
            break;

        case '-':
            tokens.push_back(punctuation(TokenKind::Minus));
            break;

        case '|':
            tokens.push_back(punctuation(TokenKind::Pipe));
            break;

        case '(':
            tokens.push_back(punctuation(TokenKind::LParen));
            break;

        case ')':
            tokens.push_back(punctuation(TokenKind::RParen));
            break;

        case '[':
            tokens.push_back(punctuation(TokenKind::LBracket));
            break;

        case ']':
            tokens.push_back(punctuation(TokenKind::RBracket));
            break;

        case ',':
            tokens.push_back(punctuation(TokenKind::Comma));
            break;

        case '"':
            tokens.push_back(readString());
            break;

        default:
            // Consume it so we cannot loop forever, but do not pretend the file
            // was understood.
            diags_.error(line_, column_,
                         "unrecognised character '" + std::string(1, c) + "'");
            get();
            break;
        }
    }

    tokens.push_back({TokenKind::EndOfFile, "", line_, column_});
    return tokens;
}

} // namespace dbc_parser
