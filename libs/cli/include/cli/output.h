#ifndef CLI_OUTPUT_H_
#define CLI_OUTPUT_H_

#include <spdlog/fmt/fmt.h>

#include <cstdio>
#include <utility>

namespace cli
{

// Tool RESULTS go to stdout, unadorned. Diagnostics stay on SPDLOG_*.
//
// This is a deliberate carve-out from the tree's "never std::cout" rule, and the
// reason is that the rule is about *logging*. A command-line tool's answer is
// not a log line, and routing it through spdlog made it unusable:
//
//     $ inspect list
//     [2026/08/05 22:35:20.667-07:00] [info] [10840267:list.cpp:50] vehicle/engine/rpm ...
//
// which cannot be piped into jq, cut, or anything else. The timestamp, level and
// source location are exactly right for "something happened" and exactly wrong
// for "here is what you asked for".
//
// So: the answer goes here, on stdout. Everything about *how* the answer was
// obtained -- a subscription that failed, a schema that was not in the registry,
// a sample that was dropped -- stays on SPDLOG_*, which writes to stderr, so
// `inspect list --json > topics.json` still shows its warnings on the terminal
// and still produces a parseable file.
template <typename... Args>
void out(fmt::format_string<Args...> format, Args&&... args)
{
    fmt::print(stdout, format, std::forward<Args>(args)...);
    std::fputc('\n', stdout);
}

// Same, without the newline, for callers assembling a line in pieces.
template <typename... Args>
void outPartial(fmt::format_string<Args...> format, Args&&... args)
{
    fmt::print(stdout, format, std::forward<Args>(args)...);
}

// stdout is block-buffered when it is not a terminal, so a long-running verb
// that prints a table every second (`watch`, `hz`) shows nothing at all until
// the buffer fills if its output is piped. Anything that prints on a timer has
// to call this; anything that prints once and exits does not, because exit
// flushes.
inline void flush()
{
    std::fflush(stdout);
}

}  // namespace cli

#endif  // CLI_OUTPUT_H_
