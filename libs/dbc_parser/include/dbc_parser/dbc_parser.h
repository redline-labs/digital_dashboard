#pragma once

#include "dbc_parser/ast.h"
#include "dbc_parser/dbc_lexer.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dbc_parser {

struct ParseError {
    int line{0};
    int column{0};
    std::string message{};
};

class Parser {
public:
    explicit Parser(std::string_view input);

    std::optional<Database> parse(ParseError &errorOut);

private:
    const Token &peek() const;
    const Token &get();
    bool eof() const;
    bool accept(TokenKind kind);
    bool expect(TokenKind kind, ParseError &err, std::string_view what);

    bool parseVersion(Database &db, ParseError &err);
    bool parseNodes(Database &db, ParseError &err);
    bool parseMessage(Database &db, ParseError &err);
    bool parseSignal(Message &msg, ParseError &err);
    bool parseComment(Database &db, ParseError &err);
    bool parseValueTable(Database &db, ParseError &err);
    bool parseNamespaceSection(Database &db, ParseError &err);


private:
    std::vector<Token> tokens_{};
    size_t index_{0};
    // (no longer used) messageId -> (signalName -> comment)
    std::unordered_map<uint32_t, std::unordered_map<std::string, std::string>> signalCommentsByMsgId_{};
};

} // namespace dbc_parser


