#ifndef DBCCODGEN_GENERATE_H_H
#define DBCCODGEN_GENERATE_H_H

#include "dbc_parser/ast.h"

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace dbc_codegen
{

// One generated file, held in memory.
struct GeneratedFile
{
    std::string name{};    // file name only, no directory
    std::string content{};
};

// Generate everything for a database, without touching the filesystem.
//
// Generation being a pure function of the parsed database is what lets it be
// driven from a DBC held in a string literal -- so the shape of the generated
// text (string escaping, enumerator naming, which C++ type a signal gets) can
// be tested without a file, a build step or a compiler.
//
// Testing generated *behaviour* still needs a real file, because the compiler
// only consumes files. That is what the synthetic DBCs under tests/dbcs/ are.
std::vector<GeneratedFile> generate_sources(const dbc_parser::Database &db,
                                            const std::string &base);

// Write what generate_sources() produced into outputDir.
void write_sources(const std::vector<GeneratedFile> &files,
                   const std::filesystem::path &outputDir);

// Individual emitters, each writing to a stream. Exposed for tests that want
// to look at one file in isolation.
void generate_cpp_common_header(const std::string &base, std::ostream &out);
void generate_cpp_message_header(const dbc_parser::Message &message, const std::string &base,
                                 std::ostream &out);
void generate_cpp_header(const dbc_parser::Database &db, const std::string &base,
                         std::ostream &out);
void generate_cpp_parser_header(const dbc_parser::Database &db, const std::string &base,
                                std::ostream &out);
void generate_cpp_parser_source(const dbc_parser::Database &db, const std::string &base,
                                std::ostream &out);

} // namespace dbc_codegen

#endif // DBCCODGEN_GENERATE_H_H
