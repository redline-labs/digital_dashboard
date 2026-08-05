#pragma once

#include "dbc_parser/ast.h"
#include "dbc_parser/dbc_lexer.h"
#include "dbc_parser/diagnostic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbc_parser
{

class Parser
{
  public:
    explicit Parser(std::string_view input);

    // Returns nothing if the file could not be understood. Anything that would
    // change what the generated code decodes is an error, not a warning: a DBC
    // we only half understand produces a decoder that is confidently wrong.
    std::optional<Database> parse();

    const Diagnostics &diagnostics() const;

  private:
    const Token &peek() const;
    const Token &peekAhead(size_t offset) const;
    const Token &get();
    bool eof() const;
    bool accept(TokenKind kind);
    bool expect(TokenKind kind, std::string_view context);

    void errorHere(std::string message);
    void warnHere(std::string message);
    void skipToNextLine();

    bool takeUint(uint32_t &out, std::string_view what);
    bool takeInt64(int64_t &out, std::string_view what);
    bool takeDouble(double &out, std::string_view what);
    bool takeIdentifier(std::string &out, std::string_view what);
    bool takeString(std::string &out, std::string_view what);

    void parseVersion(Database &db);
    void parseNodes(Database &db);
    void parseMessage(Database &db);
    bool parseSignal(Message &msg);
    void parseComment();
    void parseValueTable(Database &db);
    void parseNamedValueTable(Database &db);
    void parseSignalValueType();
    void parseAttribute();
    void parseAttributeDefinition();
    void parseSkippedSection(std::string_view keyword);
    bool parseValueMappings(std::vector<ValueMapping> &out);

    // CM_, VAL_, SIG_VALTYPE_ and BA_ all refer back to a BO_ by id. They are
    // collected while scanning and applied once the whole file is read, so the
    // parser does not depend on section order and can say "no such signal"
    // instead of dropping the reference on the floor.
    struct PendingComment
    {
        std::string kind;
        uint32_t messageId{};
        std::string signalName;
        std::string text;
        int line{};
        int column{};
    };

    struct PendingValues
    {
        uint32_t messageId{};
        std::string signalName;
        std::string tableName; // set when VAL_ names a VAL_TABLE_
        std::vector<ValueMapping> mappings;
        int line{};
        int column{};
    };

    struct PendingValueType
    {
        uint32_t messageId{};
        std::string signalName;
        SignalValueType type{SignalValueType::Integer};
        int line{};
        int column{};
    };

    struct PendingAttribute
    {
        std::string name;
        uint32_t messageId{};
        std::string value;
        int line{};
        int column{};
    };

    void resolvePending(Database &db);
    void validate(Database &db);
    void validateMessage(const Message &msg, int line);
    void validateSignal(const Message &msg, const Signal &sig, int line);

    Message *findMessage(Database &db, uint32_t id);

    std::vector<Token> tokens_{};
    size_t index_{0};
    Diagnostics diags_{};

    std::vector<PendingComment> pendingComments_{};
    std::vector<PendingValues> pendingValues_{};
    std::vector<PendingValueType> pendingValueTypes_{};
    std::vector<PendingAttribute> pendingAttributes_{};

    // Line each message id was defined on, for "duplicate message" diagnostics.
    std::vector<std::pair<uint32_t, int>> messageDefinitionLines_{};
};

// True if `name` can be used verbatim as a C++ identifier. Message and signal
// names become struct, member and enum names in generated code, so a name that
// is a keyword or contains punctuation has to be rejected here rather than
// surfacing as a syntax error inside a generated header.
bool isUsableIdentifier(std::string_view name);

} // namespace dbc_parser
