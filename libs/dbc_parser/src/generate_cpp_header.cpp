#include "dbc_parser/generate_h.h"
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/bundled/ostream.h>
#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <filesystem>

namespace dbc_codegen
{

static std::string_view determine_decoded_type_for_signal(const dbc_parser::Signal& signal)
{
    if (signal.valueTable.empty() == false)
    {
        return "Values";
    }

    if (signal.scale != 1.0)
    {
        // TODO super hack.
        return "double";
    }
    
    // TODO super hack.
    if (signal.isSigned == true)
    {
        return "int64_t";
    }
    else
    {
        return "uint64_t";
    }
}

static void generate_message_header(const dbc_parser::Message& message, const std::string& base, std::ofstream& out)
{
    //  Find the name of the multiplexor signal, and then create a method to return a ref for it.
    const dbc_parser::Signal* muxSigPtr = nullptr;
    for (const auto& s : message.signals)
    {
        if (s.isMultiplexor)
        {
            muxSigPtr = &s;
            break;
        }
    }

    // Find the valid multiplexed group indexes.
    std::set<uint32_t> valid_mux_group_indexes;
    for (const auto& s : message.signals)
    {
        if (s.isMultiplex)
        {
            valid_mux_group_indexes.insert(s.multiplexedGroupIdx);
        }
    }

    // Lets assume the lowest multiplexed group index is the "start" of each batch.
    // We'll latch onto this multiplexor index before we start marking the multiplexed group indexes as seen.
    uint32_t start_mux_group_index = valid_mux_group_indexes.empty() ? 0 : *std::min_element(valid_mux_group_indexes.begin(), valid_mux_group_indexes.end());

    // Create header guard
    std::string guard = base + "_" + message.name + "_H_";
    std::transform(guard.begin(), guard.end(), guard.begin(), ::toupper);
    
    fmt::print(out, "#ifndef {}\n", guard);
    fmt::print(out, "#define {}\n\n", guard);
    fmt::print(out, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(out, "#include <array>\n");
    fmt::print(out, "#include <cstdint>\n");
    fmt::print(out, "#include <cstdbool>\n");
    fmt::print(out, "#include <string_view>\n");
    fmt::print(out, "#include <limits>\n");
    fmt::print(out, "#include <cmath>\n");
    fmt::print(out, "\n");

    fmt::print(out, "namespace {}\n", base);
    fmt::print(out, "{{\n");

    fmt::print(out, "struct {}_t\n", message.name);
    fmt::print(out, "{{\n");
    fmt::print(out, "    static constexpr std::string_view name = \"{}\";\n", message.name);
    fmt::print(out, "    static constexpr uint32_t id = 0x{:08X}u;\n", message.id);
    fmt::print(out, "    static constexpr uint8_t dlc = {}u;\n", message.dlc);
    fmt::print(out, "    static constexpr std::string_view transmitter = \"{}\";\n", message.transmitter);
    fmt::print(out, "    static constexpr std::string_view comment = \"{}\";\n", message.comment);
    fmt::print(out, "\n");
    fmt::print(out, "    static constexpr size_t signal_count = {}u;\n", message.signals.size());
    fmt::print(out, "\n");
    fmt::print(out, "    static constexpr bool is_multiplexed = {};\n", message.isMultiplexed);
    
    if (message.isMultiplexed == true)
    {
        fmt::print(out, "    static constexpr std::string_view mutiplexor_name = \"{}\";\n", muxSigPtr->name);
        fmt::print(out, "    static constexpr std::array<uint32_t, {}> multiplexor_group_indexes = {{{}}};\n", valid_mux_group_indexes.size(), fmt::join(valid_mux_group_indexes, ", "));
        fmt::print(out, "    static constexpr uint32_t start_mux_group_index = {}u;\n", start_mux_group_index);
    }

    fmt::print(out, "\n");
    fmt::print(out, "    static constexpr std::array<std::string_view, {}u> signal_names =\n", message.signals.size());
    fmt::print(out, "    {{\n");
    for (const auto& signal : message.signals)
    {
        fmt::print(out, "        \"{}\",\n", signal.name);
    }
    fmt::print(out, "    }};\n");
    fmt::print(out, "\n");

    for (const auto& signal : message.signals)
    {
        fmt::print(out, "    struct sig_{}_t\n", signal.name);
        fmt::print(out, "    {{\n");
        fmt::print(out, "        static constexpr std::string_view name = \"{}\";\n", signal.name);
        fmt::print(out, "        static constexpr std::string_view comment = \"{}\";\n", signal.comment);
        fmt::print(out, "\n");
        fmt::print(out, "        static constexpr uint32_t start_bit = {}u;\n", signal.startBit);
        fmt::print(out, "        static constexpr uint32_t length = {}u;\n", signal.length);
        fmt::print(out, "        static constexpr bool little_endian = {};\n", signal.littleEndian);
        fmt::print(out, "        static constexpr bool is_signed = {};\n", signal.isSigned);
        fmt::print(out, "\n");
        fmt::print(out, "        static constexpr bool is_multiplex = {};\n", signal.isMultiplex);
        fmt::print(out, "        static constexpr bool is_multiplexor = {};\n", signal.isMultiplexor);
        fmt::print(out, "        static constexpr uint32_t multiplexed_group_idx = {}u;\n", signal.multiplexedGroupIdx);
        fmt::print(out, "\n");
        fmt::print(out, "        static constexpr bool has_value_table = {};\n", signal.valueTable.empty() == false);
        fmt::print(out, "\n");

        if (signal.valueTable.empty() == false)
        {
            fmt::print(out, "        enum class Values\n");
            fmt::print(out, "        {{\n");
            for (const auto& value : signal.valueTable)
            {
                fmt::print(out, "            {} = {},\n", value.description, value.rawValue);
            }

            fmt::print(out, "        }};\n");
        }
        else
        {
            // TODO: Find a better way that keep everything as doubles all the time.
            fmt::print(out, "        static constexpr double scale = {};\n", signal.scale);
            fmt::print(out, "        static constexpr double offset = {};\n", signal.offset);
            fmt::print(out, "        static constexpr double minimum = {};\n", signal.minimum);
            fmt::print(out, "        static constexpr double maximum = {};\n", signal.maximum);
        }

        fmt::print(out, "\n");
        fmt::print(out, "        // The type to be used for the decoded value of this signal.\n");
        fmt::print(out, "        using Type = {};\n", determine_decoded_type_for_signal(signal));
        fmt::print(out, "\n");

        fmt::print(out, "\n");
        fmt::print(out, "        static constexpr std::string_view unit = \"{}\";\n", signal.unit);
        fmt::print(out, "        static constexpr std::array<std::string_view, {}> receivers =\n", signal.receivers.size());
        fmt::print(out, "        {{\n");
        for (const auto& receiver : signal.receivers)
        {
            fmt::print(out, "            \"{}\",\n", receiver);
        }
        fmt::print(out, "        }};\n");
        fmt::print(out, "\n");

        fmt::print(out, "    }};\n");
        fmt::print(out, "\n");
    }

    // Declare the fields within the struct.
    for (const auto& signal : message.signals)
    {
        fmt::print(out, "    sig_{}_t::Type {};\n", signal.name, signal.name);   
    }
    fmt::print(out, "\n");

    // If the message is multiplexed, keep a bool for each of the multiplexed group indexes.
    if (message.isMultiplexed == true)
    {
        fmt::print(out, "    // Keep a bool for when we observe each of the multiplexed group indexes.\n");
        for (const auto& group_index : valid_mux_group_indexes)
        {
            fmt::print(out, "    bool seen_mux_{};\n", group_index);
        }
        fmt::print(out, "\n");
    }

    // Declare the struct constructor
    fmt::print(out, "    constexpr {}_t(){}\n", message.name, message.signals.size() > 0 ? " :" : "");
    
    for (size_t i = 0; i < message.signals.size(); ++i)
    {
        const auto& signal = message.signals[i];
        bool last = (i == message.signals.size() - 1) && (message.isMultiplexed == false);

        fmt::print(out, "      {}{{}}{}\n", signal.name, last ? "" : ",");  //  Brace initialization for all the signals.
    }

    if (message.isMultiplexed == true)
    {
        size_t i = 0;
        for (const auto& group_index : valid_mux_group_indexes)
        {
            bool last = (i == valid_mux_group_indexes.size() - 1);
            ++i;

            fmt::print(out, "      seen_mux_{}{{false}}{}\n", group_index, last ? "" : ",");
        }
    }

    fmt::print(out, "    {{\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    if (message.isMultiplexed == true)
    {
        // Helper to get the value of the multiplexor signal.
        if (muxSigPtr)
        {
            fmt::print(out, "    sig_{}_t::Type& mux()\n", muxSigPtr->name);
            fmt::print(out, "    {{\n");
            fmt::print(out, "        return {};\n", muxSigPtr->name);
            fmt::print(out, "    }}\n");
            fmt::print(out, "\n");
        }
    }

    // Helpers for encode: to_raw_u and insert_bits
    fmt::print(out, "    template <typename Sig>\n");
    fmt::print(out, "    static constexpr uint64_t to_raw_u(typename Sig::Type value)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        if constexpr (Sig::has_value_table)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            return static_cast<uint64_t>(static_cast<int64_t>(value));\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            const double raw_d = (static_cast<double>(value) - static_cast<double>(Sig::offset)) / static_cast<double>(Sig::scale);\n");
    fmt::print(out, "            const double rounded = (raw_d >= 0.0) ? std::floor(raw_d + 0.5) : std::ceil(raw_d - 0.5);\n");
    fmt::print(out, "            int64_t raw = static_cast<int64_t>(rounded);\n");
    fmt::print(out, "            if constexpr (Sig::is_signed)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t bits = Sig::length;\n");
    fmt::print(out, "                const int64_t minv = (bits == 64u) ? std::numeric_limits<int64_t>::min() : (-(1ll << (bits - 1u)));\n");
    fmt::print(out, "                const int64_t maxv = (bits == 64u) ? std::numeric_limits<int64_t>::max() : ((1ll << (bits - 1u)) - 1ll);\n");
    fmt::print(out, "                if (raw < minv) raw = minv;\n");
    fmt::print(out, "                if (raw > maxv) raw = maxv;\n");
    fmt::print(out, "                const uint64_t mask = (bits == 64u) ? ~0ull : ((1ull << bits) - 1ull);\n");
    fmt::print(out, "                return static_cast<uint64_t>(raw) & mask;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "            else\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t bits = Sig::length;\n");
    fmt::print(out, "                const uint64_t maxu = (bits == 64u) ? ~0ull : ((1ull << bits) - 1ull);\n");
    fmt::print(out, "                uint64_t raw_u = (raw < 0) ? 0ull : static_cast<uint64_t>(raw);\n");
    fmt::print(out, "                if (raw_u > maxu) raw_u = maxu;\n");
    fmt::print(out, "                return raw_u;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    template <typename Sig, size_t N2>\n");
    fmt::print(out, "    static constexpr void insert_bits(std::array<uint8_t, N2>& buf, uint64_t raw_u)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        if constexpr (Sig::little_endian)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t absBit = Sig::start_bit + i;\n");
    fmt::print(out, "                const uint32_t byteIndex = absBit / 8u;\n");
    fmt::print(out, "                const uint32_t bitIndex = absBit % 8u;\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((raw_u >> i) & 0x1u);\n");
    fmt::print(out, "                buf[byteIndex] = static_cast<uint8_t>((buf[byteIndex] & static_cast<uint8_t>(~(1u << bitIndex))) | (bit << bitIndex));\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            uint32_t absBit = Sig::start_bit;\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t byteIndex = absBit / 8u;\n");
    fmt::print(out, "                const uint32_t bitIndex = absBit % 8u;\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((raw_u >> (Sig::length - 1u - i)) & 0x1u);\n");
    fmt::print(out, "                buf[byteIndex] = static_cast<uint8_t>((buf[byteIndex] & static_cast<uint8_t>(~(1u << bitIndex))) | (bit << bitIndex));\n");
    fmt::print(out, "                if (bitIndex == 0u) absBit += 15u; else absBit -= 1u;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Create the encode function for the message struct.
    fmt::print(out, "    constexpr std::array<uint8_t, {}u> encode() const\n", message.dlc);
    fmt::print(out, "    {{\n");
    fmt::print(out, "        std::array<uint8_t, {}u> data = {{{{}}}};\n", message.dlc);
    fmt::print(out, "\n");
    if (message.isMultiplexed)
    {
        if (muxSigPtr != nullptr)
        {
            fmt::print(out, "        uint64_t raw_signal = to_raw_u<sig_{}_t>({});\n", muxSigPtr->name, muxSigPtr->name);
            fmt::print(out, "        insert_bits<sig_{}_t>(data, raw_signal);\n", muxSigPtr->name);
            fmt::print(out, "\n");
            // Conditionally place multiplexed signals grouped by multiplex index
            for (const auto& group_index : valid_mux_group_indexes)
            {
                fmt::print(out, "        if ({} == {})\n", muxSigPtr->name, group_index);
                fmt::print(out, "        {{\n");
                for (const auto& signal : message.signals)
                {
                    if ((signal.isMultiplex == false) || (signal.isMultiplexor == true))
                    {
                        continue;
                    }
                    if (signal.multiplexedGroupIdx != group_index)
                    {
                        continue;
                    }

                    fmt::print(out, "            raw_signal = to_raw_u<sig_{}_t>({});\n", signal.name, signal.name);
                    fmt::print(out, "            insert_bits<sig_{}_t>(data, raw_signal);\n", signal.name);
                    fmt::print(out, "\n");
                }
                fmt::print(out, "        }}\n");
                fmt::print(out, "\n");
            }
            // Non-multiplexed signals
            for (const auto& signal : message.signals)
            {
                if (!signal.isMultiplex && !signal.isMultiplexor)
                {
                    fmt::print(out, "        {{ const uint64_t raw_u = to_raw_u<sig_{}_t>({}); insert_bits<sig_{}_t>(data, raw_u); }}\n", signal.name, signal.name, signal.name);
                }
            }
        }
    }
    else
    {
        for (const auto& signal : message.signals)
        {
            fmt::print(out, "        {{ const uint64_t raw_u = to_raw_u<sig_{}_t>({}); insert_bits<sig_{}_t>(data, raw_u); }}\n", signal.name, signal.name, signal.name);
        }
    }

    fmt::print(out, "\n");
    fmt::print(out, "        return data;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Generic extractor templated on signal type; returns Sig::Type
    fmt::print(out, "    template <typename Sig, size_t N>\n");
    fmt::print(out, "    static constexpr typename Sig::Type extract(const std::array<uint8_t, N>& data)\n");
    fmt::print(out, "    {{\n");
    fmt::print(out, "        static_assert(Sig::length <= 64, \"Signal length must be less than or equal to 64\");\n");
    fmt::print(out, "        uint64_t raw_u = 0;\n");
    fmt::print(out, "        if constexpr (Sig::little_endian)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t absBit = Sig::start_bit + i;\n");
    fmt::print(out, "                const uint32_t byteIndex = absBit / 8u;\n");
    fmt::print(out, "                const uint32_t bitIndex = absBit % 8u;\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((data[byteIndex] >> bitIndex) & 0x1u);\n");
    fmt::print(out, "                raw_u |= (static_cast<uint64_t>(bit) << i);\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            // Motorola/big-endian: walk bits decreasing within byte, wrapping to previous byte at boundaries.\n");
    fmt::print(out, "            uint32_t absBit = Sig::start_bit;\n");
    fmt::print(out, "            for (uint32_t i = 0; i < Sig::length; ++i)\n");
    fmt::print(out, "            {{\n");
    fmt::print(out, "                const uint32_t byteIndex = absBit / 8u;\n");
    fmt::print(out, "                const uint32_t bitIndex = absBit % 8u;\n");
    fmt::print(out, "                const uint8_t bit = static_cast<uint8_t>((data[byteIndex] >> bitIndex) & 0x1u);\n");
    fmt::print(out, "                raw_u = (raw_u << 1) | static_cast<uint64_t>(bit);\n");
    fmt::print(out, "                if (bitIndex == 0u) absBit += 15u; else absBit -= 1u;\n");
    fmt::print(out, "            }}\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "        int64_t raw = static_cast<int64_t>(raw_u);\n");
    fmt::print(out, "        if constexpr (Sig::is_signed && Sig::length > 0 && Sig::length < 64)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            if ((raw_u >> (Sig::length - 1u)) & 0x1u) raw |= (~0ll) << Sig::length;\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        if constexpr (Sig::has_value_table)\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            return static_cast<typename Sig::Type>(static_cast<int64_t>(raw));\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "        else\n");
    fmt::print(out, "        {{\n");
    fmt::print(out, "            return static_cast<typename Sig::Type>(static_cast<double>(raw) * static_cast<double>(Sig::scale) + static_cast<double>(Sig::offset));\n");
    fmt::print(out, "        }}\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Create the decode function for the message struct using extract
    fmt::print(out, "    constexpr bool decode(const std::array<uint8_t, {}u>& data)\n", message.dlc);
    fmt::print(out, "    {{\n");

    if (message.isMultiplexed)
    {
        fmt::print(out, "        {} = extract<sig_{}_t>(data);\n", muxSigPtr->name, muxSigPtr->name);
        fmt::print(out, "\n");

        for (const auto& group_index : valid_mux_group_indexes)
        {
            fmt::print(out, "        if ({} == {}u)\n", muxSigPtr->name, group_index);
            fmt::print(out, "        {{\n");
            
            // If this is the first group index, mark it as seen and clear out the seen flags
            // for all the other group indexes.  This is to align our decoding to the start
            // of the batch.
            if (group_index == start_mux_group_index)
            {
                fmt::print(out, "            // Special case for the first group index.  Clear the seen flags for all the other group indexes.\n");
                fmt::print(out, "            seen_mux_{} = true;\n", group_index);
                for (const auto& seen_idx : valid_mux_group_indexes)
                {
                    if (seen_idx != start_mux_group_index)
                    {
                        fmt::print(out, "            seen_mux_{} = false;\n", seen_idx);
                    }
                }
            }
            else
            {
                fmt::print(out, "            seen_mux_{} = true;\n", group_index);
            }
            fmt::print(out, "\n");

            for (const auto& signal : message.signals)
            {
                if ((signal.isMultiplex == false) || (signal.isMultiplexor == true))
                {
                    continue;
                }

                if (signal.multiplexedGroupIdx != group_index)
                {
                    continue;
                }

                fmt::print(out, "            {} = extract<sig_{}_t>(data);\n", signal.name, signal.name);
            }

            fmt::print(out, "        }}\n");
            fmt::print(out, "\n");
        }

        for (const auto& signal : message.signals)
        {
            if ((signal.isMultiplex == false) && (signal.isMultiplexor == false))
            {
                fmt::print(out, "        {} = extract<sig_{}_t>(data);\n", signal.name, signal.name);
            }
        }
    }
    else
    {
        for (const auto& signal : message.signals)
        {
            fmt::print(out, "        {} = extract<sig_{}_t>(data);\n", signal.name, signal.name);
        }
    }
    fmt::print(out, "        return true;\n");
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Visit all signals by (name, value-ref, type-tag)
    fmt::print(out, "    template <typename Func>\n");
    fmt::print(out, "    constexpr void visit(Func&& fn)\n");
    fmt::print(out, "    {{\n");
    for (const auto& signal : message.signals)
    {
        fmt::print(out, "        fn({}, sig_{}_t{{}});\n", signal.name, signal.name);
    }
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");
    fmt::print(out, "    template <typename Func>\n");
    fmt::print(out, "    constexpr void visit(Func&& fn) const\n");
    fmt::print(out, "    {{\n");
    for (const auto& signal : message.signals)
    {
        fmt::print(out, "        fn({}, sig_{}_t{{}});\n", signal.name, signal.name);
    }
    fmt::print(out, "    }}\n");
    fmt::print(out, "\n");

    // Create a method that returns true if all the multiplexed group indexes have been seen.
    if (message.isMultiplexed == true)
    {
        fmt::print(out, "    constexpr bool all_multiplexed_indexes_seen() const\n");
        fmt::print(out, "    {{\n");
        fmt::print(out, "        return ");
        size_t i = 0;
        for (const auto& group_index : valid_mux_group_indexes)
        {
            bool last = (i == valid_mux_group_indexes.size() - 1);
            ++i;
            fmt::print(out, "seen_mux_{}{}", group_index, last ? "" : " && ");
        }

        fmt::print(out, ";\n");
        fmt::print(out, "    }}\n");
        fmt::print(out, "\n");


        // Create a method that clears all the flags for the multiplexed group indexes.
        fmt::print(out, "    constexpr void clear_seen_multiplexed_indexes()\n");
        fmt::print(out, "    {{\n");
        for (const auto& group_index : valid_mux_group_indexes)
        {
            fmt::print(out, "        seen_mux_{} = false;\n", group_index);
        }
        fmt::print(out, "    }}\n");
        fmt::print(out, "\n");
    }

    fmt::print(out, "}};  // struct {}_t\n", message.name);
    fmt::print(out, "\n");
    fmt::print(out, "}}  // namespace {}\n", base);
    fmt::print(out, "\n");
    
    // Close header guard (reuse the guard variable from the beginning)
    fmt::print(out, "#endif  // {}\n", guard);
}

// Minimal placeholder: will emit C header based on Database
void generate_cpp_header(const dbc_parser::Database &db, const std::string &base, const std::filesystem::path &outputDir)
{
    std::string base_upper = base;
    std::transform(base_upper.begin(), base_upper.end(), base_upper.begin(), ::toupper);

    // Create individual message headers
    for (const auto& message : db.messages)
    {
        std::filesystem::path msgHeaderPath = outputDir / (base + "_" + message.name + ".h");
        SPDLOG_INFO("Writing message header: {}", msgHeaderPath.string());
        
        std::ofstream msgOut(msgHeaderPath, std::ios::out | std::ios::trunc);
        if (!msgOut)
        {
            SPDLOG_ERROR("Failed to open message header for writing: {}", msgHeaderPath.string());
            throw std::runtime_error("Failed to open message header for writing");
        }
        
        generate_message_header(message, base, msgOut);
    }

    // Create main header that includes all message headers
    std::filesystem::path hPath = outputDir / (base + ".h");
    SPDLOG_INFO("Writing main header: {}", hPath.string());
    
    std::ofstream hout(hPath, std::ios::out | std::ios::trunc);
    if (!hout)
    {
        SPDLOG_ERROR("Failed to open main header for writing: {}", hPath.string());
        throw std::runtime_error("Failed to open main header for writing");
    }

    fmt::print(hout, "#ifndef {}_H_\n", base_upper);
    fmt::print(hout, "#define {}_H_\n", base_upper);
    fmt::print(hout, "\n");
    fmt::print(hout, "/* Generated C++ header - do not edit as any changes will be overwritten. */\n");
    fmt::print(hout, "#include <array>\n");
    fmt::print(hout, "#include <cstdint>\n");
    fmt::print(hout, "#include <string_view>\n");
    fmt::print(hout, "\n");

    // Include all message headers
    for (const auto& message : db.messages)
    {
        fmt::print(hout, "#include \"{}_{}.h\"\n", base, message.name);
    }
    fmt::print(hout, "\n");

    fmt::print(hout, "namespace {}\n", base);
    fmt::print(hout, "{{\n");

    // Extract all the message ids.
    std::set<uint32_t> message_ids;
    for (const auto& message : db.messages)
    {
        message_ids.insert(message.id);
    }

    // ------------------------------------------------------------
    // Create a overall struct for the database.
    // ------------------------------------------------------------
    fmt::print(hout, "struct {}_t\n", base);
    fmt::print(hout, "{{\n");
    fmt::print(hout, "    static constexpr std::string_view name = \"{}\";\n", base);
    fmt::print(hout, "    static constexpr std::array<uint32_t, {}u> message_ids = {{{:#08x}}};\n", message_ids.size(), fmt::join(message_ids, ", "));
    fmt::print(hout, "\n");

    // Create an enum for all the messages present in attempt to normalize against message Ids.
    fmt::print(hout, "    enum class Messages : uint32_t\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        Unknown = 0,\n");
    for (const auto& message : db.messages)
    {
        fmt::print(hout, "        {} = {:#08x},\n", message.name, message.id);
    }
    fmt::print(hout, "    }};\n");
    fmt::print(hout, "\n");

    // Actual instances of the messages.
    for (const auto& message : db.messages)
    {
        fmt::print(hout, "    {}_t {};\n", message.name, message.name);
    }

    fmt::print(hout, "\n");

    // Create the constructor for the overall struct.
    fmt::print(hout, "    constexpr {}_t() :\n", base);
    for (size_t i = 0; i < db.messages.size(); ++i)
    {
        const auto& message = db.messages[i];
        bool last = i == db.messages.size() - 1;
        fmt::print(hout, "      {}{{}}{}\n", message.name, last == true ? "" : ",");
    }
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "\n");

    // Create the decode function for the overall struct.
    fmt::print(hout, "    // Decode a message from the database.\n");
    fmt::print(hout, "    // Returns true if the message was decoded, false otherwise.\n");
    fmt::print(hout, "    constexpr Messages decode(uint32_t message_id, const std::array<uint8_t, 8u>& data)\n", base);
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        Messages decoded = Messages::Unknown;\n");
    fmt::print(hout, "\n");

    for (size_t i = 0; i < db.messages.size(); ++i)
    {
        const auto& message = db.messages[i];
        
        if (i == 0)
        {
            fmt::print(hout, "        if (message_id == {}_t::id)\n", message.name);
        }
        else
        {
            fmt::print(hout, "        else if (message_id == {}_t::id)\n", message.name);
        }
        fmt::print(hout, "        {{\n");
        fmt::print(hout, "            {}.decode(data);\n", message.name);
        fmt::print(hout, "            decoded = Messages::{};\n", message.name);
        fmt::print(hout, "        }}\n");
    }

    fmt::print(hout, "    \n");
    fmt::print(hout, "        return decoded;\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "\n");

    // Create a method that accepts a Messages enum and returns the string name of the message.
    fmt::print(hout, "    static constexpr std::string_view get_message_name(Messages msg) noexcept\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        switch (msg)\n");
    fmt::print(hout, "        {{\n");
    for (const auto& message : db.messages)
    {
        fmt::print(hout, "        case Messages::{}:\n", message.name);
        fmt::print(hout, "            return \"{}\";\n", message.name);
        fmt::print(hout, "\n");
    }
    fmt::print(hout, "        case Messages::Unknown:\n");
    fmt::print(hout, "        default:\n");
    fmt::print(hout, "            return \"Unknown\";\n");
    fmt::print(hout, "        }}\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "\n");

    // Create a method that accepts a message ID  and returns the string name of the message.
    fmt::print(hout, "    static constexpr std::string_view get_message_name(uint32_t message_id) noexcept\n");
    fmt::print(hout, "    {{\n");
    fmt::print(hout, "        switch (message_id)\n");
    fmt::print(hout, "        {{\n");
    for (const auto& message : db.messages)
    {
        fmt::print(hout, "        case {}_t::id:\n", message.name);
        fmt::print(hout, "            return \"{}\";\n", message.name);
        fmt::print(hout, "\n");
    }
    fmt::print(hout, "        default:\n");
    fmt::print(hout, "            return \"Unknown\";\n");
    fmt::print(hout, "        }}\n");
    fmt::print(hout, "    }}\n");
    fmt::print(hout, "\n");

    fmt::print(hout, "}};\n");
    fmt::print(hout, "}}  // namespace {}\n", base);

    fmt::print(hout, "#endif  // {}_H_\n", base_upper);
}

}  // namespace dbc_codegen

