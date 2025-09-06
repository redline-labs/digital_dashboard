#include "dbc_parser/dbc_lexer.h"

#include <cctype>

#include <spdlog/spdlog.h>

namespace dbc_parser
{

Lexer::Lexer(std::string_view input) :
    input_(input),
    index_(0),
    line_(1),
    column_(1)
{
}

char Lexer::peek() const
{
    if (index_ >= input_.size())
    {
        return '\0';
    }
    return input_[index_];
}

char Lexer::get() {
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
        if (c == '/' && index_ + 1 < input_.size() && input_[index_ + 1] == '/')
        {
            while (!eof() && peek() != '\n') get();
            continue;
        }
        break;
    }
}

void Lexer::consumeNewline()
{
    if (peek() == '\n')
    {
        get();
    }
}

Token Lexer::readIdentifier()
{
    Token tok{TokenKind::Identifier, {}, line_, column_};
    while (!eof())
    {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')
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
    bool seenDot = false;
    while (!eof()) {
        char c = peek();
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            tok.lexeme.push_back(get());
        }
        else if (c == '.' && !seenDot)
        {
            seenDot = true;
            tok.lexeme.push_back(get());
        }
        else if (c == 'e' || c == 'E')
        {
            tok.lexeme.push_back(get());
            if (peek() == '+' || peek() == '-')
            {
                tok.lexeme.push_back(get());
            }
        }
        else
        {
            break;
        }
    }
    return tok;
}

Token Lexer::readString()
{
    Token tok{TokenKind::String, {}, line_, column_};
    // consume opening quote
    get();
    while (!eof())
    {
        char c = get();
        if (c == '"')
        {
            break;
        }
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
            Token t{TokenKind::Newline, "\n", line_, column_};
            get();
            tokens.push_back(std::move(t));
            continue;
        }

        char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' )
        {
            tokens.push_back(readIdentifier());
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            tokens.push_back(readNumber());
            continue;
        }

        if (c == '-' && index_ + 1 < input_.size() && std::isdigit(static_cast<unsigned char>(input_[index_ + 1])))
        {
            // negative number
            Token t{TokenKind::Number, "-", line_, column_};
            get();
            Token rest = readNumber();
            t.lexeme += rest.lexeme;
            t.kind = TokenKind::Number;
            tokens.push_back(std::move(t));
            continue;
        }
    
        switch (c)
        {
            case ':':
                tokens.push_back({TokenKind::Colon, std::string(1, get()), line_, column_});
                break;

            case ';':
                tokens.push_back({TokenKind::Semicolon, std::string(1, get()), line_, column_});
                break;

            case '@':
                tokens.push_back({TokenKind::At, std::string(1, get()), line_, column_});
                break;

            case '+':
                tokens.push_back({TokenKind::Plus, std::string(1, get()), line_, column_});
                break;

            case '-':
                tokens.push_back({TokenKind::Minus, std::string(1, get()), line_, column_});
                break;

            case '|':
                tokens.push_back({TokenKind::Pipe, std::string(1, get()), line_, column_});
                break;

            case '(':
                tokens.push_back({TokenKind::LParen, std::string(1, get()), line_, column_});
                break;

            case ')':
                tokens.push_back({TokenKind::RParen, std::string(1, get()), line_, column_});
                break;

            case '[':
                tokens.push_back({TokenKind::LBracket, std::string(1, get()), line_, column_});
                break;

            case ']':
                tokens.push_back({TokenKind::RBracket, std::string(1, get()), line_, column_});
                break;

            case ',':
                tokens.push_back({TokenKind::Comma, std::string(1, get()), line_, column_});
                break;

            case '"':
                tokens.push_back(readString());
                break;

            default:
                // unrecognized char, consume to avoid infinite loop
                SPDLOG_ERROR("Unrecognized character: {} (0x{:02X}) at line {} column {}", c, static_cast<unsigned char>(c), line_, column_);
                get();
                break;
        }
    }

    tokens.push_back({TokenKind::EndOfFile, "", line_, column_});
    return tokens;
}

} // namespace dbc_parser


