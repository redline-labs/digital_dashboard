#pragma once

#include <string>
#include <vector>

namespace dbc_parser
{

// Where a problem is, and what it is. Every rejection carries one of these,
// because the failure mode this replaced was a build that silently dropped a
// message and exited 0 -- one typo'd SG_ line used to delete a whole BO_ and
// everything under it, and the only clue was whether some node later failed to
// compile against the vanished symbol.
enum class Severity
{
    Warning, // parsed, but something was ignored or assumed
    Error,   // the file does not describe what it claims to; do not generate
};

struct Diagnostic
{
    Severity severity{Severity::Error};
    int line{0};
    int column{0};
    std::string message{};
};

// Collects diagnostics for one parse. Held by the lexer and the parser so a
// single run reports everything wrong with a file rather than only the last
// thing that went wrong.
class Diagnostics
{
  public:
    void error(int line, int column, std::string message)
    {
        entries_.push_back({Severity::Error, line, column, std::move(message)});
    }

    void warn(int line, int column, std::string message)
    {
        entries_.push_back({Severity::Warning, line, column, std::move(message)});
    }

    bool hasErrors() const
    {
        for (const auto &entry : entries_)
        {
            if (entry.severity == Severity::Error)
            {
                return true;
            }
        }
        return false;
    }

    const std::vector<Diagnostic> &entries() const
    {
        return entries_;
    }

  private:
    std::vector<Diagnostic> entries_{};
};

} // namespace dbc_parser
