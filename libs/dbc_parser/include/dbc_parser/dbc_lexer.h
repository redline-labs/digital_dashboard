#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace dbc_parser
{

enum class TokenKind
{
    EndOfFile,
    Newline,
    Identifier,
    Number,
    String,
    // Punctuation
    Colon,       // :
    Semicolon,   // ;
    At,          // @
    Plus,        // +
    Minus,       // -
    Pipe,        // |
    LParen,      // (
    RParen,      // )
    LBracket,    // [
    RBracket,    // ]
    Comma,       // ,
    Quote,       // "
};

struct Token
{
    TokenKind kind{};
    std::string lexeme{};
    int line{1};
    int column{1};
};

class Lexer
{
  public:
    explicit Lexer(std::string_view input);

    // Tokenize entire input into tokens including Newline markers.
    std::vector<Token> tokenize();

  private:
    char peek() const;
    char get();
    bool eof() const;
    void skipWhitespaceExceptNewline();
    void consumeNewline();
    Token readIdentifier();
    Token readNumber();
    Token readString();

private:
    std::string_view input_;
    size_t index_;
    int line_;
    int column_;
};

} // namespace dbc_parser


