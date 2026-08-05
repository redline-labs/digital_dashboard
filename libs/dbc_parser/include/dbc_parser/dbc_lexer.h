#pragma once

#include "dbc_parser/diagnostic.h"

#include <string>
#include <string_view>
#include <vector>

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
};

// Human-readable name for a kind, for diagnostics like "expected ':', got '@'".
std::string_view describe(TokenKind kind);

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
    // Diagnostics for unrecognised characters and unterminated strings land in
    // `diags`, which must outlive the lexer.
    Lexer(std::string_view input, Diagnostics &diags);

    // Tokenize entire input into tokens including Newline markers.
    std::vector<Token> tokenize();

  private:
    char peek() const;
    char peekAhead(size_t offset) const;
    char get();
    bool eof() const;
    void skipWhitespaceExceptNewline();
    Token readIdentifier();
    Token readNumber();
    Token readString();

    // Punctuation, recorded at the position of the character itself. Reading
    // the position after consuming the character is what used to put every
    // punctuation token one column to the right of where it actually was.
    Token punctuation(TokenKind kind);

    std::string_view input_;
    Diagnostics &diags_;
    size_t index_{0};
    int line_{1};
    int column_{1};
};

} // namespace dbc_parser
