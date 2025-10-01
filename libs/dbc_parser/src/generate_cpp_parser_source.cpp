#include "dbc_parser/generate_h.h"
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace dbc_codegen
{

void generate_cpp_parser_source(const dbc_parser::Database &db, const std::string &base, const std::filesystem::path &outputDir)
{
    std::string base_upper = base;
    std::transform(base_upper.begin(), base_upper.end(), base_upper.begin(), ::toupper);

    std::filesystem::path psPath = outputDir / (base + "_parser.cpp");
    SPDLOG_INFO("Writing parser source: {}", psPath.string());

    std::ofstream out(psPath, std::ios::out | std::ios::trunc);
    if (!out)
    {
        SPDLOG_ERROR("Failed to open parser source for writing: {}", psPath.string());
        throw std::runtime_error("Failed to open parser source for writing");
    }

    fmt::print(out, "#include \"{}_parser.h\"\n\n", base);
    fmt::print(out, "namespace {}\n{{\n", base);
    
    fmt::print(out, "{}_parser::{}_parser() :\n", base, base);
    fmt::print(out, "    db_{{}},\n");
    
    for (size_t i = 0; i < db.messages.size(); ++i)
    {
        const auto& msg = db.messages[i];
        bool last = i == db.messages.size() - 1;
        fmt::print(out, "    {}_handler_{{}}{}\n", msg.name, last == true ? "" : ",");
    }

    fmt::print(out, "{{\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");
    fmt::print(out, "bool {}_parser::handle_can_frame(uint32_t id, const std::array<uint8_t, 8u>& data)\n{{\n", base);
    fmt::print(out, "    auto m = db_.decode(id, data);\n");
    fmt::print(out, "    switch (m)\n    {{\n");
    fmt::print(out, "        case {}_t::Messages::Unknown:\n", base);
    fmt::print(out, "           break;\n");
    fmt::print(out, "\n");
    for (const auto& msg : db.messages)
    {
        fmt::print(out, "        case {}_t::Messages::{}:\n", base, msg.name);
        if (msg.isMultiplexed)
        {
            fmt::print(out, "            if (db_.{}.all_multiplexed_indexes_seen() == true)\n", msg.name);
            fmt::print(out, "            {{\n");
            fmt::print(out, "                db_.{}.clear_seen_multiplexed_indexes();\n", msg.name);
            fmt::print(out, "                if ({}_handler_)\n", msg.name);
            fmt::print(out, "                {{\n");
            fmt::print(out, "                    {}_handler_(db_.{});\n", msg.name, msg.name);
            fmt::print(out, "                }}\n");
            fmt::print(out, "            }}\n");
        }
        else
        {
            fmt::print(out, "            if ({}_handler_)\n", msg.name);
            fmt::print(out, "            {{\n");
            fmt::print(out, "                {}_handler_(db_.{});\n", msg.name, msg.name);
            fmt::print(out, "            }}\n");
        }
        
        fmt::print(out, "            break;\n\n");
    }
    
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    return m != {}_t::Messages::Unknown;\n", base);
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");
    for (const auto& msg : db.messages)
    {
        fmt::print(out, "void {}_parser::on_{}({}_handler_t handler)\n", base, msg.name, msg.name);
        fmt::print(out, "{{\n");
        fmt::print(out, "    {}_handler_ = handler;\n", msg.name);
        fmt::print(out, "}}\n");
        fmt::print(out, "\n");
    }

    // Create a method that returns a reference to the database.
    fmt::print(out, "const {}_parser::db_t& {}_parser::get_db() const\n", base, base);
    fmt::print(out, "{{\n");
    fmt::print(out, "    return db_;\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");

    fmt::print(out, "}} // namespace {}\n", base);
}

}  // namespace dbc_codegen

