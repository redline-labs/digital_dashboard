#include "dbc_parser/generate_h.h"
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace dbc_codegen
{

void generate_cpp_parser_header(const dbc_parser::Database &db, const std::string &base, const std::filesystem::path &outputDir)
{
    std::string base_upper = base;
    std::transform(base_upper.begin(), base_upper.end(), base_upper.begin(), ::toupper);

    std::filesystem::path phPath = outputDir / (base + "_parser.h");
    SPDLOG_INFO("Writing parser header: {}", phPath.string());

    std::ofstream out(phPath, std::ios::out | std::ios::trunc);
    if (!out)
    {
        SPDLOG_ERROR("Failed to open parser header for writing: {}", phPath.string());
        throw std::runtime_error("Failed to open parser header for writing");
    }

    fmt::print(out, "#ifndef {}_PARSER_H_\n", base_upper);
    fmt::print(out, "#define {}_PARSER_H_\n\n", base_upper);
    fmt::print(out, "#include <array>\n");
    fmt::print(out, "#include <cstdint>\n");
    fmt::print(out, "#include <functional>\n");
    fmt::print(out, "#include <tuple>\n");
    fmt::print(out, "#include <type_traits>\n");
    fmt::print(out, "#include <algorithm>\n");
    fmt::print(out, "#include <utility>\n");
    fmt::print(out, "#include <vector>\n\n");
    fmt::print(out, "#include \"{}.h\"\n\n", base);

    fmt::print(out, "namespace {}\n{{\n", base);
    // Base class so the parser can own aggregators of any Messages pack
    fmt::print(out, "struct aggregator_base\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");
    fmt::print(out, "class {}_parser\n{{\n", base);
    fmt::print(out, "  public:\n");
    fmt::print(out, "    using db_t = {}_t;\n", base);
    for (const auto& msg : db.messages)
    {
        fmt::print(out, "    using {}_handler_t = std::function<void(const {}_t&)>;\n", msg.name, msg.name);
    }
    fmt::print(out, "\n");
    fmt::print(out, "    {}_parser();\n", base);
    fmt::print(out, "    bool handle_can_frame(uint32_t id, const std::array<uint8_t, 8u>& data);\n\n");
    fmt::print(out, "    template <{}_t::Messages... Ms>\n", base);
    fmt::print(out, "    void add_message_aggregator(std::function<void(const {}_t&)> on_complete);\n\n", base);
    for (const auto& msg : db.messages)
    {
        fmt::print(out, "    void on_{}({}_handler_t handler);\n", msg.name, msg.name);
    }
    fmt::print(out, "    const {}_parser::db_t& get_db() const;\n\n", base);
    fmt::print(out, "\n  private:\n");
    fmt::print(out, "    db_t db_;\n");
    for (const auto& msg : db.messages)
    {
        fmt::print(out, "    {}_handler_t {}_handler_;\n", msg.name, msg.name);
    }
    fmt::print(out, "    std::vector<std::unique_ptr<aggregator_base>> aggregators_;\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");

    // Registration helper by enum value (Messages)
    fmt::print(out, "template <{}_t::Messages M>\n", base);
    fmt::print(out, "struct MessageRegistrarById;\n");
    fmt::print(out, "\n");
    for (const auto& msg : db.messages)
    {
        fmt::print(out, "template <>\n");
        fmt::print(out, "struct MessageRegistrarById<{}_t::Messages::{}>\n", base, msg.name);
        fmt::print(out, "{{\n");
        fmt::print(out, "    template <typename ParserT, typename Fn>\n");
        fmt::print(out, "    static void attach(ParserT& p, Fn&& fn)\n");
        fmt::print(out, "    {{\n");
        fmt::print(out, "        p.on_{}(std::forward<Fn>(fn));\n", msg.name);
        fmt::print(out, "    }}\n");
        fmt::print(out, "}};\n");
        fmt::print(out, "\n");
    }

    // Aggregator that takes Messages enum values as template non-type parameters
    fmt::print(out, "template <{}_t::Messages... Ms>\n", base);
    fmt::print(out, "class message_aggregator : public aggregator_base\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "  public:\n");
    fmt::print(out, "    using OnComplete = std::function<void(const {}_t&)>;\n\n", base);
    fmt::print(out, "    message_aggregator({}_parser& parser, OnComplete on_complete) :\n", base);
    fmt::print(out, "      db_ref_{{parser.get_db()}},\n");
    fmt::print(out, "      seen_{{false}},\n");
    fmt::print(out, "      on_complete_{{std::move(on_complete)}}\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        register_all(parser, std::make_index_sequence<sizeof...(Ms)>{{}});\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    void reset()\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        seen_.fill(false);\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "  private:\n");
    fmt::print(out, "    template <std::size_t I> void mark_seen_index()\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        static_assert(I < sizeof...(Ms));\n");
    fmt::print(out, "        // We want to align to the first message.  so only mark other messages as received if the first has been received.\n");
    fmt::print(out, "        seen_[I] = I == 0 ? true : seen_[0];\n");
    fmt::print(out, "        if (std::all_of(seen_.begin(), seen_.end(), [](bool b){{ return b; }}))\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            if (on_complete_)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                on_complete_(db_ref_);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "            reset();\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    template <std::size_t... I> void register_all({}_parser& parser, std::index_sequence<I...>)\n", base);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        (MessageRegistrarById<Ms>::attach(parser, [this](const auto&) {{ mark_seen_index<I>(); }}), ...);\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    const {}_t& db_ref_;\n", base);
    fmt::print(out, "    std::array<bool, sizeof...(Ms)> seen_;\n");
    fmt::print(out, "    OnComplete on_complete_;\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");
    // Inline template method to add and own an aggregator inside the parser
    fmt::print(out, "template <{}_t::Messages... Ms>\n", base);
    fmt::print(out, "inline void {}_parser::add_message_aggregator(std::function<void(const {}_t&)> on_complete)\n", base, base);
    fmt::print(out, "{{\n");
    fmt::print(out, "    using Agg = message_aggregator<Ms...>;\n");
    fmt::print(out, "    aggregators_.push_back(std::make_unique<Agg>(*this, std::move(on_complete)));\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");

    fmt::print(out, "}} // namespace {}\n\n", base);
    fmt::print(out, "#endif // {}_PARSER_H_\n", base_upper);
}

}  // namespace dbc_codegen

