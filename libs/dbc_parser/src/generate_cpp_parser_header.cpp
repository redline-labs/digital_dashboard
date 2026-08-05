#include "dbc_parser/generate_h.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <ostream>

namespace dbc_codegen
{

void generate_cpp_parser_header(const dbc_parser::Database &db, const std::string &base,
                                std::ostream &out)
{
    std::string baseUpper = base;
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(), ::toupper);

    fmt::print(out, "#ifndef {}_PARSER_H_\n", baseUpper);
    fmt::print(out, "#define {}_PARSER_H_\n\n", baseUpper);
    fmt::print(out, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(out, "#include <algorithm>\n");
    fmt::print(out, "#include <array>\n");
    fmt::print(out, "#include <cstdint>\n");
    fmt::print(out, "#include <functional>\n");
    fmt::print(out, "#include <memory>\n");
    fmt::print(out, "#include <span>\n");
    fmt::print(out, "#include <type_traits>\n");
    fmt::print(out, "#include <utility>\n");
    fmt::print(out, "#include <vector>\n\n");
    fmt::print(out, "#include \"{}.h\"\n\n", base);

    fmt::print(out, "namespace {}\n{{\n", base);
    fmt::print(out, "\n");
    fmt::print(out, "// Base class so the parser can own aggregators of any Messages pack.\n");
    fmt::print(out, "struct aggregator_base\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "    virtual ~aggregator_base() = default;\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");
    fmt::print(out, "// Not thread safe, and not meant to be: register every handler before\n");
    fmt::print(out, "// frames start arriving, then feed it from one thread.\n");
    fmt::print(out, "class {}_parser\n{{\n", base);
    fmt::print(out, "  public:\n");
    fmt::print(out, "    using db_t = {}_t;\n", base);
    for (const auto &msg : db.messages)
    {
        fmt::print(out, "    using {}_handler_t = std::function<void(const {}_t&)>;\n", msg.name,
                   msg.name);
    }
    fmt::print(out, "\n");
    fmt::print(out, "    {}_parser() = default;\n", base);
    fmt::print(out, "\n");
    fmt::print(out, "    // True if the frame was one of ours and long enough to decode.\n");
    fmt::print(out, "    bool handle_can_frame(uint32_t id, std::span<const uint8_t> data);\n");
    fmt::print(out, "\n");
    fmt::print(out, "    template <{}_t::Messages... Ms>\n", base);
    fmt::print(out, "    void add_message_aggregator(std::function<void(const {}_t&)> on_complete);\n\n",
               base);

    fmt::print(out, "    // Handlers accumulate: registering a second one for a message adds it\n");
    fmt::print(out, "    // rather than replacing the first. Assignment used to mean an\n");
    fmt::print(out, "    // aggregator silently threw away a directly registered handler for\n");
    fmt::print(out, "    // the same message, with nothing to say it had happened.\n");
    for (const auto &msg : db.messages)
    {
        fmt::print(out, "    void on_{}({}_handler_t handler);\n", msg.name, msg.name);
    }
    fmt::print(out, "\n");

    // The every-frame variant only differs for multiplexed messages, but is
    // generated for all of them so callers do not have to know which is which.
    fmt::print(out, "    // Fires for every accepted frame. For a multiplexed message that is\n");
    fmt::print(out, "    // once per multiplex group rather than once per complete batch, which\n");
    fmt::print(out, "    // is what you want from a device that only sends some groups.\n");
    for (const auto &msg : db.messages)
    {
        fmt::print(out, "    void on_{}_each_frame({}_handler_t handler);\n", msg.name, msg.name);
    }
    fmt::print(out, "\n");
    fmt::print(out, "    const db_t& get_db() const;\n");
    fmt::print(out, "\n");
    fmt::print(out, "  private:\n");
    fmt::print(out, "    db_t db_{{}};\n");
    for (const auto &msg : db.messages)
    {
        fmt::print(out, "    std::vector<{}_handler_t> {}_handlers_{{}};\n", msg.name, msg.name);
        fmt::print(out, "    std::vector<{}_handler_t> {}_frame_handlers_{{}};\n", msg.name,
                   msg.name);
    }
    fmt::print(out, "    std::vector<std::unique_ptr<aggregator_base>> aggregators_{{}};\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");

    fmt::print(out, "template <{}_t::Messages M>\n", base);
    fmt::print(out, "struct MessageRegistrarById;\n");
    fmt::print(out, "\n");
    for (const auto &msg : db.messages)
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

    fmt::print(out, "template <{}_t::Messages... Ms>\n", base);
    fmt::print(out, "class message_aggregator : public aggregator_base\n");
    fmt::print(out, "{{\n");
    fmt::print(out, "  public:\n");
    fmt::print(out, "    using OnComplete = std::function<void(const {}_t&)>;\n\n", base);
    fmt::print(out, "    message_aggregator({}_parser& parser, OnComplete on_complete) :\n", base);
    fmt::print(out, "      db_ref_{{parser.get_db()}},\n");
    fmt::print(out, "      seen_{{}},\n");
    fmt::print(out, "      on_complete_{{std::move(on_complete)}}\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        seen_.fill(false);\n");
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
    fmt::print(out, "        // Align to the first message of the group: the others only count\n");
    fmt::print(out, "        // once it has been seen, so a batch cannot be assembled from\n");
    fmt::print(out, "        // halves of two different cycles.\n");
    fmt::print(out, "        seen_[I] = (I == 0) ? true : seen_[0];\n");
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
    fmt::print(out, "    template <std::size_t... I> void register_all({}_parser& parser, std::index_sequence<I...>)\n",
               base);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        (MessageRegistrarById<Ms>::attach(parser, [this](const auto&) {{ mark_seen_index<I>(); }}), ...);\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    const {}_t& db_ref_;\n", base);
    fmt::print(out, "    std::array<bool, sizeof...(Ms)> seen_;\n");
    fmt::print(out, "    OnComplete on_complete_;\n");
    fmt::print(out, "}};\n");
    fmt::print(out, "\n");
    fmt::print(out, "template <{}_t::Messages... Ms>\n", base);
    fmt::print(out, "inline void {}_parser::add_message_aggregator(std::function<void(const {}_t&)> on_complete)\n",
               base, base);
    fmt::print(out, "{{\n");
    fmt::print(out, "    using Agg = message_aggregator<Ms...>;\n");
    fmt::print(out, "    aggregators_.push_back(std::make_unique<Agg>(*this, std::move(on_complete)));\n");
    fmt::print(out, "}}\n");
    fmt::print(out, "\n");

    fmt::print(out, "}} // namespace {}\n\n", base);
    fmt::print(out, "#endif // {}_PARSER_H_\n", baseUpper);
}

} // namespace dbc_codegen
