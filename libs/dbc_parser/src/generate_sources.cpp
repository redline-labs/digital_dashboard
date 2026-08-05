#include "dbc_parser/generate_h.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dbc_codegen
{
namespace
{

GeneratedFile render(std::string name, const auto &emit)
{
    std::ostringstream out;
    emit(out);
    return GeneratedFile{std::move(name), out.str()};
}

} // namespace

std::vector<GeneratedFile> generate_sources(const dbc_parser::Database &db, const std::string &base)
{
    std::vector<GeneratedFile> files;
    files.reserve(db.messages.size() + 4);

    files.push_back(render(base + "_common.h",
                           [&](std::ostream &out) { generate_cpp_common_header(base, out); }));

    for (const auto &message : db.messages)
    {
        files.push_back(render(base + "_" + message.name + ".h", [&](std::ostream &out) {
            generate_cpp_message_header(message, base, out);
        }));
    }

    files.push_back(render(base + ".h",
                           [&](std::ostream &out) { generate_cpp_header(db, base, out); }));
    files.push_back(render(base + "_parser.h",
                           [&](std::ostream &out) { generate_cpp_parser_header(db, base, out); }));
    files.push_back(render(base + "_parser.cpp",
                           [&](std::ostream &out) { generate_cpp_parser_source(db, base, out); }));

    return files;
}

void write_sources(const std::vector<GeneratedFile> &files, const std::filesystem::path &outputDir)
{
    for (const auto &file : files)
    {
        const std::filesystem::path path = outputDir / file.name;
        SPDLOG_INFO("Writing {}", path.string());

        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out)
        {
            SPDLOG_ERROR("Failed to open {} for writing", path.string());
            throw std::runtime_error("Failed to open " + path.string() + " for writing");
        }

        out << file.content;
        if (!out)
        {
            SPDLOG_ERROR("Failed to write {}", path.string());
            throw std::runtime_error("Failed to write " + path.string());
        }
    }
}

} // namespace dbc_codegen
