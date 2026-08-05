#include "dbc_parser/generate_h.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <ostream>

namespace dbc_codegen
{

void generate_cpp_parser_source(const dbc_parser::Database &db, const std::string &base,
                                std::ostream &out)
{
    fmt::print(out, "/* Generated C++ source - do not edit as any changes will be overwritten. */\n");
    fmt::print(out, "#include \"{}_parser.h\"\n\n", base);
    fmt::print(out, "namespace {}\n{{\n", base);
    fmt::print(out, "\n");

    fmt::print(out, "bool {}_parser::handle_can_frame(uint32_t id, std::span<const uint8_t> data)\n{{\n",
               base);
    fmt::print(out, "    const {}_t::Messages decoded = db_.decode(id, data);\n", base);
    fmt::print(out, "\n");
    fmt::print(out, "    switch (decoded)\n    {{\n");
    fmt::print(out, "    case {}_t::Messages::Unknown:\n", base);
    fmt::print(out, "        break;\n");
    fmt::print(out, "\n");

    for (const auto &msg : db.messages)
    {
        fmt::print(out, "    case {}_t::Messages::{}:\n", base, msg.name);
        fmt::print(out, "        for (const auto& handler : {}_frame_handlers_)\n", msg.name);
        fmt::print(out, "        {{\n");
        fmt::print(out, "            handler(db_.{});\n", msg.name);
        fmt::print(out, "        }}\n");

        if (msg.isMultiplexed)
        {
            fmt::print(out, "\n");
            fmt::print(out, "        if (db_.{}.all_multiplexed_indexes_seen())\n", msg.name);
            fmt::print(out, "        {{\n");
            fmt::print(out, "            db_.{}.clear_seen_multiplexed_indexes();\n", msg.name);
            fmt::print(out, "            for (const auto& handler : {}_handlers_)\n", msg.name);
            fmt::print(out, "            {{\n");
            fmt::print(out, "                handler(db_.{});\n", msg.name);
            fmt::print(out, "            }}\n");
            fmt::print(out, "        }}\n");
        }
        else
        {
            fmt::print(out, "        for (const auto& handler : {}_handlers_)\n", msg.name);
            fmt::print(out, "        {{\n");
            fmt::print(out, "            handler(db_.{});\n", msg.name);
            fmt::print(out, "        }}\n");
        }

        fmt::print(out, "        break;\n\n");
    }

    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    return decoded != {}_t::Messages::Unknown;\n", base);
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");

    for (const auto &msg : db.messages)
    {
        fmt::print(out, "void {}_parser::on_{}({}_handler_t handler)\n", base, msg.name, msg.name);
        fmt::print(out, "{{\n");
        fmt::print(out, "    {}_handlers_.push_back(std::move(handler));\n", msg.name);
        fmt::print(out, "}}\n");
        fmt::print(out, "\n");
        fmt::print(out, "void {}_parser::on_{}_each_frame({}_handler_t handler)\n", base, msg.name,
                   msg.name);
        fmt::print(out, "{{\n");
        fmt::print(out, "    {}_frame_handlers_.push_back(std::move(handler));\n", msg.name);
        fmt::print(out, "}}\n");
        fmt::print(out, "\n");
    }

    fmt::print(out, "const {}_parser::db_t& {}_parser::get_db() const\n", base, base);
    fmt::print(out, "{{\n");
    fmt::print(out, "    return db_;\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");
    fmt::print(out, "}} // namespace {}\n", base);
}

} // namespace dbc_codegen
