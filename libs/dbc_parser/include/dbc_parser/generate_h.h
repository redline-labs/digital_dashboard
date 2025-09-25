#ifndef DBCCODGEN_GENERATE_H_H
#define DBCCODGEN_GENERATE_H_H

#include "dbc_parser/ast.h"

#include <fstream>
#include <string>

namespace dbc_codegen
{

// Emit C header file for serialization/deserialization based on DBC
void generate_cpp_header(const dbc_parser::Database &db, const std::string &base, std::ofstream &out);

// Emit C++ parser header and source that wrap decoding and callbacks per message
void generate_cpp_parser_header(const dbc_parser::Database &db, const std::string &base, std::ofstream &out);
void generate_cpp_parser_source(const dbc_parser::Database &db, const std::string &base, std::ofstream &out);

}

#endif // DBCCODGEN_GENERATE_H_H
