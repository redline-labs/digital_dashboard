// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turn an EDS into C++ that knows the device.
//
// Everything emitted here is derived from the file. That is worth stating
// because the previous generator did not manage it: two parser bugs meant the
// COB-ID lookups always failed, so the "derived" values were the DS401
// fallbacks, and the PDO packers were fixed text that happened to agree with
// 0x1600/0x1601/0x1A00 without ever consulting them. Both looked correct. The
// way to keep them correct is to have nothing to fall back to, so a lookup that
// fails here is a hard error rather than a constant.
//
// The generated header carries, for each PDO the file declares, a struct whose
// fields are the mapped objects under their own names, plus pack/unpack against
// the mapping's real bit offsets.

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include "canopen/eds_parser.h"
#include "canopen/pdo_mapping.h"

#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// ============================================================================
// Identifier construction
// ============================================================================

// "Digital Output Indicators 1 through 8" -> "digital_output_indicators_1_through_8".
// Runs of anything that is not alphanumeric collapse to a single underscore, so
// a name with punctuation does not produce a run of them.
std::string to_snake_case(std::string_view name)
{
    std::string out;
    bool pendingSeparator = false;
    for (char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            if (pendingSeparator && !out.empty())
            {
                out.push_back('_');
            }
            pendingSeparator = false;
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        else
        {
            pendingSeparator = true;
        }
    }
    if (out.empty())
    {
        return "unnamed";
    }
    // "1st Receive PDO Communication Parameter" cannot become an identifier
    // starting with a digit. An `n` in front is the usual way out, and it
    // keeps the name recognisably the one in the file.
    if (std::isdigit(static_cast<unsigned char>(out[0])) != 0)
    {
        out.insert(out.begin(), 'n');
    }
    return out;
}

std::string to_screaming_snake_case(std::string_view name)
{
    std::string out = to_snake_case(name);
    for (char& c : out)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// Names come from the EDS, and an EDS is free to give two objects the same one
// -- 0x1600's eight mapping entries are all called "PDO Mapping Entry". A
// generated struct cannot have two fields of the same name, so collisions get a
// numeric suffix in declaration order.
class NameSet
{
public:
    std::string unique(std::string candidate)
    {
        if (used_.insert(candidate).second)
        {
            return candidate;
        }
        for (int suffix = 2;; ++suffix)
        {
            std::string attempt = fmt::format("{}_{}", candidate, suffix);
            if (used_.insert(attempt).second)
            {
                return attempt;
            }
        }
    }

private:
    std::set<std::string> used_;
};

// ============================================================================
// Type mapping
// ============================================================================

struct FieldType
{
    std::string cppType;
    bool isSigned { false };
};

// The C++ type a mapped field gets. Width comes from the mapping word, so a
// 4-bit slice of an UNSIGNED8 lands in a uint8_t; signedness comes from the
// target object's DataType, because that is the only thing that says whether
// the bits are two's complement.
FieldType field_type_for(canopen::DataType dataType, uint8_t bits)
{
    const bool isSigned = dataType == canopen::DataType::Integer8
        || dataType == canopen::DataType::Integer16 || dataType == canopen::DataType::Integer24
        || dataType == canopen::DataType::Integer32 || dataType == canopen::DataType::Integer40
        || dataType == canopen::DataType::Integer48 || dataType == canopen::DataType::Integer56
        || dataType == canopen::DataType::Integer64;

    const char* width = "64";
    if (bits <= 8) width = "8";
    else if (bits <= 16) width = "16";
    else if (bits <= 32) width = "32";

    return FieldType { fmt::format("{}int{}_t", isSigned ? "" : "u", width), isSigned };
}

// ============================================================================
// PDO description
// ============================================================================

struct PdoField
{
    std::string name;
    FieldType type;
    uint16_t index { 0 };
    uint8_t sub { 0 };
    uint8_t bits { 0 };
    uint8_t bitOffset { 0 };
    std::string comment;
};

struct PdoDescription
{
    // "rpdo1", "tpdo1" -- how the device's own documentation refers to them.
    std::string name;
    // "Rpdo1"
    std::string structName;
    uint16_t mappingIndex { 0 };
    uint16_t communicationIndex { 0 };
    bool isTransmit { false };
    uint8_t lengthBytes { 0 };
    std::string parameterName;
    std::vector<PdoField> fields;
};

// PDO numbering is positional: 0x1400 is RPDO1, 0x1401 is RPDO2, 0x1800 is
// TPDO1. "Transmit" is from the device's point of view, so a TPDO is one we
// receive and an RPDO is one we send.
std::optional<PdoDescription> describe_pdo(const canopen::ObjectDictionary& od,
                                           uint16_t mappingIndex,
                                           std::vector<std::string>& errors)
{
    const uint16_t communicationIndex = canopen::communication_index_for_mapping(mappingIndex);
    const bool isTransmit = canopen::is_tpdo_mapping_index(mappingIndex);
    const uint16_t number = static_cast<uint16_t>(
        (communicationIndex - (isTransmit ? 0x1800 : 0x1400)) + 1);

    if (od.get(communicationIndex) == nullptr)
    {
        errors.push_back(fmt::format(
            "[{:04X}] has no communication parameter object [{:04X}]; the PDO cannot be addressed",
            mappingIndex, communicationIndex));
        return std::nullopt;
    }

    auto mapping = canopen::read_pdo_mapping(od, mappingIndex);
    for (const auto& problem : mapping.problems)
    {
        errors.push_back(problem);
    }
    if (mapping.entries.empty())
    {
        errors.push_back(fmt::format("[{:04X}] maps nothing", mappingIndex));
        return std::nullopt;
    }

    PdoDescription pdo;
    pdo.name = fmt::format("{}pdo{}", isTransmit ? "t" : "r", number);
    pdo.structName = fmt::format("{}pdo{}", isTransmit ? 'T' : 'R', number);
    pdo.mappingIndex = mappingIndex;
    pdo.communicationIndex = communicationIndex;
    pdo.isTransmit = isTransmit;
    pdo.lengthBytes = mapping.lengthBytes();
    const canopen::Object* mappingObject = od.get(mappingIndex);
    pdo.parameterName = mappingObject != nullptr ? mappingObject->parameterName : std::string {};

    NameSet fieldNames;
    for (const auto& entry : mapping.entries)
    {
        const canopen::SubObject* target = od.get(entry.index, entry.sub);
        if (target == nullptr)
        {
            errors.push_back(fmt::format("[{:04X}] maps 0x{:04X}:{:02X}, which is not declared",
                                         mappingIndex, entry.index, entry.sub));
            return std::nullopt;
        }

        PdoField field;
        field.name = fieldNames.unique(to_snake_case(target->parameterName));
        field.type = field_type_for(target->dataType, entry.bits);
        field.index = entry.index;
        field.sub = entry.sub;
        field.bits = entry.bits;
        field.bitOffset = entry.bitOffset;
        field.comment = fmt::format("0x{:04X}:{:02X} {} '{}'", entry.index, entry.sub,
                                    canopen::to_string(target->dataType), target->parameterName);
        pdo.fields.push_back(std::move(field));
    }

    return pdo;
}

// ============================================================================
// COB-IDs
// ============================================================================

// The COB-ID base for a communication parameter object, with the CiA control
// bits masked off. Every one of these is written `$NODEID+0x40000180` in the
// file, so this is where a parser that cannot read a $NODEID expression used to
// silently produce nothing and get a hardcoded constant in its place.
std::optional<uint32_t> cobid_base(const canopen::ObjectDictionary& od, uint16_t communicationIndex,
                                   std::vector<std::string>& errors)
{
    const canopen::SubObject* entry = od.get(communicationIndex, 1);
    if (entry == nullptr || !entry->defaultValue.has_value())
    {
        errors.push_back(
            fmt::format("[{:04X}sub1] has no COB-ID default to derive from", communicationIndex));
        return std::nullopt;
    }

    // Resolved at node 0 so what comes out is the base the node ID is added to.
    auto resolved = od.defaultValue(communicationIndex, 1, 0);
    if (!resolved.has_value())
    {
        errors.push_back(fmt::format("[{:04X}sub1] default is not a number or a $NODEID expression",
                                     communicationIndex));
        return std::nullopt;
    }

    const uint32_t base = static_cast<uint32_t>(*resolved & 0x1FFFFFFF);
    if (base == 0)
    {
        errors.push_back(
            fmt::format("[{:04X}sub1] resolves to COB-ID 0", communicationIndex));
        return std::nullopt;
    }
    return base;
}

// ============================================================================
// Emission
// ============================================================================

class Writer
{
public:
    explicit Writer(const std::string& path)
        : out_(path)
    {
        if (!out_)
        {
            throw std::runtime_error("cannot open for writing: " + path);
        }
    }

    template <typename... Args>
    void line(fmt::format_string<Args...> format, Args&&... args)
    {
        out_ << fmt::format(format, std::forward<Args>(args)...) << '\n';
    }

    void blank() { out_ << '\n'; }

private:
    std::ofstream out_;
};

void emit_banner(Writer& writer, const std::string& input)
{
    // The basename, not the path we were given: the build passes an absolute
    // one, and baking a build directory into a generated file makes the output
    // differ between machines for no reason.
    const size_t slash = input.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? input : input.substr(slash + 1);

    writer.line("// SPDX-License-Identifier: GPL-3.0-or-later");
    writer.line("//");
    writer.line("// Generated by canopen_code_gen from {}.", name);
    writer.line("// Do not edit: change the EDS instead.");
}

void emit_helpers_header(const std::string& path, const std::string& base, const std::string& input,
                         const canopen::ObjectDictionary& od,
                         const std::vector<PdoDescription>& pdos,
                         const std::map<std::string, uint32_t>& cobids)
{
    Writer writer(path);
    emit_banner(writer, input);
    writer.line("#pragma once");
    writer.blank();
    writer.line("#include <array>");
    writer.line("#include <cstdint>");
    writer.blank();
    writer.line("#include \"canopen/pdo_bits.h\"");
    writer.line("#include \"helpers/can_frame.h\"");
    writer.blank();
    writer.line("namespace {}", base);
    writer.line("{{");
    writer.line("using CanFrame = helpers::CanFrame;");
    writer.blank();

    // --- device identity ---------------------------------------------------
    writer.line("// From [DeviceInfo].");
    writer.line("inline constexpr uint32_t VENDOR_ID = 0x{:08X};", od.deviceInfo.vendorNumber);
    writer.line("inline constexpr uint32_t PRODUCT_CODE = 0x{:08X};", od.deviceInfo.productNumber);
    writer.line("inline constexpr uint32_t REVISION_NUMBER = 0x{:08X};",
                od.deviceInfo.revisionNumber);
    writer.line("inline constexpr bool LSS_SUPPORTED = {};",
                od.deviceInfo.lssSupported ? "true" : "false");
    writer.blank();

    writer.line("// Bit rates the device declares, in kbit/s. A rate absent from this list is one");
    writer.line("// the EDS says the device does not support -- asking for it is how a");
    writer.line("// reconfiguration ends with an unreachable node.");
    writer.line("inline constexpr uint32_t SUPPORTED_BITRATES_KBPS[] = {{");
    bool anyBitrate = false;
    for (const auto& [rate, supported] : od.deviceInfo.supportedBitrates)
    {
        if (supported)
        {
            writer.line("    {},", rate);
            anyBitrate = true;
        }
    }
    if (!anyBitrate)
    {
        writer.line("    0,  // the file declares none");
    }
    writer.line("}};");
    writer.blank();

    // --- object indices ----------------------------------------------------
    writer.line("// Every object the file declares, under its own name.");
    writer.line("namespace idx");
    writer.line("{{");
    {
        NameSet names;
        for (const auto& [index, object] : od.objects)
        {
            const std::string name = names.unique(to_screaming_snake_case(object.parameterName));
            writer.line("inline constexpr uint16_t {} = 0x{:04X};", name, index);
        }
    }
    writer.line("}} // namespace idx");
    writer.blank();

    // --- COB-IDs -----------------------------------------------------------
    writer.line("// COB-ID bases, resolved from the $NODEID expressions in the communication");
    writer.line("// parameter objects. Add the node ID to reach a particular device.");
    for (const auto& [name, base_id] : cobids)
    {
        writer.line("inline constexpr uint32_t COBID_{}_BASE = 0x{:03X};",
                    to_screaming_snake_case(name), base_id);
        writer.line("inline uint32_t cobid_{}(uint8_t node)", name);
        writer.line("{{");
        writer.line("    return COBID_{}_BASE + node;", to_screaming_snake_case(name));
        writer.line("}}");
        writer.blank();
    }

    // --- PDOs --------------------------------------------------------------
    for (const auto& pdo : pdos)
    {
        writer.line("// {} -- {}", pdo.name, pdo.parameterName);
        writer.line("//");
        writer.line("// Mapped by [{:04X}], addressed by [{:04X}]. {} byte{} on the wire.",
                    pdo.mappingIndex, pdo.communicationIndex, pdo.lengthBytes,
                    pdo.lengthBytes == 1 ? "" : "s");
        writer.line("// {} by the device, so we {} it.", pdo.isTransmit ? "Transmitted" : "Received",
                    pdo.isTransmit ? "unpack" : "pack");
        writer.line("struct {}", pdo.structName);
        writer.line("{{");
        for (const auto& field : pdo.fields)
        {
            writer.line("    // {}", field.comment);
            writer.line("    {} {} {{}};", field.type.cppType, field.name);
        }
        writer.line("}};");
        writer.blank();

        writer.line("inline constexpr uint8_t {}_LENGTH = {};", to_screaming_snake_case(pdo.name),
                    pdo.lengthBytes);
        writer.blank();

        writer.line("inline CanFrame pack_{}(const {}& value, uint8_t node)", pdo.name,
                    pdo.structName);
        writer.line("{{");
        writer.line("    CanFrame frame {{}};");
        writer.line("    frame.id = cobid_{}(node);", pdo.name);
        writer.line("    frame.len = {};", pdo.lengthBytes);
        for (const auto& field : pdo.fields)
        {
            writer.line("    canopen::set_bits(frame.data, {}, {}, "
                        "static_cast<uint64_t>(value.{}));",
                        field.bitOffset, field.bits, field.name);
        }
        writer.line("    return frame;");
        writer.line("}}");
        writer.blank();

        writer.line("inline {} unpack_{}(const CanFrame& frame)", pdo.structName, pdo.name);
        writer.line("{{");
        writer.line("    {} value {{}};", pdo.structName);
        for (const auto& field : pdo.fields)
        {
            if (field.type.isSigned)
            {
                writer.line("    value.{} = static_cast<{}>(canopen::sign_extend("
                            "canopen::get_bits(frame.data_span(), {}, {}), {}));",
                            field.name, field.type.cppType, field.bitOffset, field.bits,
                            field.bits);
            }
            else
            {
                writer.line("    value.{} = static_cast<{}>(canopen::get_bits(frame.data_span(), {}, {}));",
                            field.name, field.type.cppType, field.bitOffset, field.bits);
            }
        }
        writer.line("    return value;");
        writer.line("}}");
        writer.blank();
    }

    writer.line("}} // namespace {}", base);
}

void emit_node_header(const std::string& path, const std::string& base, const std::string& input,
                      const std::vector<PdoDescription>& pdos)
{
    Writer writer(path);
    emit_banner(writer, input);
    writer.line("#pragma once");
    writer.blank();
    writer.line("#include <cstdint>");
    writer.line("#include <functional>");
    writer.blank();
    writer.line("#include \"{}_helpers.h\"", base);
    writer.line("#include \"helpers/can_frame.h\"");
    writer.blank();
    writer.line("namespace {}", base);
    writer.line("{{");
    writer.blank();
    writer.line("// Demultiplexes this device's PDOs by COB-ID. It knows nothing about SDO, NMT");
    writer.line("// or heartbeat -- those are canopen::Node's business, and a frame this class");
    writer.line("// does not recognise is left for it by returning false.");
    writer.line("class node");
    writer.line("{{");
    writer.line("public:");
    writer.line("    explicit node(uint8_t node_id);");
    writer.blank();
    writer.line("    uint8_t node_id() const {{ return node_id_; }}");
    writer.blank();
    writer.line("    // True when the frame was one of this device's PDOs and was dispatched.");
    writer.line("    bool handle_frame(const helpers::CanFrame& frame);");
    writer.blank();
    for (const auto& pdo : pdos)
    {
        if (!pdo.isTransmit)
        {
            continue;
        }
        writer.line("    void on_{}(std::function<void(const {}&)> callback);", pdo.name,
                    pdo.structName);
    }
    writer.blank();
    for (const auto& pdo : pdos)
    {
        if (pdo.isTransmit)
        {
            continue;
        }
        writer.line("    // Builds the frame; sending it is the caller's job.");
        writer.line("    helpers::CanFrame make_{}(const {}& value) const;", pdo.name,
                    pdo.structName);
    }
    writer.blank();
    writer.line("private:");
    writer.line("    uint8_t node_id_;");
    for (const auto& pdo : pdos)
    {
        if (!pdo.isTransmit)
        {
            continue;
        }
        writer.line("    std::function<void(const {}&)> {}_callback_;", pdo.structName, pdo.name);
    }
    writer.line("}};");
    writer.blank();
    writer.line("}} // namespace {}", base);
}

void emit_node_source(const std::string& path, const std::string& base, const std::string& input,
                      const std::vector<PdoDescription>& pdos)
{
    Writer writer(path);
    emit_banner(writer, input);
    writer.line("#include \"{}_node.h\"", base);
    writer.blank();
    writer.line("namespace {}", base);
    writer.line("{{");
    writer.blank();
    writer.line("node::node(uint8_t node_id)");
    writer.line("    : node_id_(node_id)");
    writer.line("{{");
    writer.line("}}");
    writer.blank();

    for (const auto& pdo : pdos)
    {
        if (!pdo.isTransmit)
        {
            continue;
        }
        writer.line("void node::on_{}(std::function<void(const {}&)> callback)", pdo.name,
                    pdo.structName);
        writer.line("{{");
        writer.line("    {}_callback_ = std::move(callback);", pdo.name);
        writer.line("}}");
        writer.blank();
    }

    for (const auto& pdo : pdos)
    {
        if (pdo.isTransmit)
        {
            continue;
        }
        writer.line("helpers::CanFrame node::make_{}(const {}& value) const", pdo.name,
                    pdo.structName);
        writer.line("{{");
        writer.line("    return pack_{}(value, node_id_);", pdo.name);
        writer.line("}}");
        writer.blank();
    }

    writer.line("bool node::handle_frame(const helpers::CanFrame& frame)");
    writer.line("{{");
    writer.line("    // 11-bit identifier only: the flag bits a driver may set above it are not");
    writer.line("    // part of the COB-ID.");
    writer.line("    const uint32_t id = frame.id & 0x7FFu;");
    for (const auto& pdo : pdos)
    {
        if (!pdo.isTransmit)
        {
            continue;
        }
        writer.blank();
        writer.line("    if (id == (cobid_{}(node_id_) & 0x7FFu))", pdo.name);
        writer.line("    {{");
        writer.line("        if (frame.len < {})", pdo.lengthBytes);
        writer.line("        {{");
        writer.line("            // Short frame: the mapping says {} bytes. Dropping it is",
                    pdo.lengthBytes);
        writer.line("            // better than decoding fields out of uninitialised bytes.");
        writer.line("            return false;");
        writer.line("        }}");
        writer.line("        if ({}_callback_)", pdo.name);
        writer.line("        {{");
        writer.line("            {}_callback_(unpack_{}(frame));", pdo.name, pdo.name);
        writer.line("        }}");
        writer.line("        return true;");
        writer.line("    }}");
    }
    writer.blank();
    writer.line("    return false;");
    writer.line("}}");
    writer.blank();
    writer.line("}} // namespace {}", base);
}

} // namespace

int main(int argc, char** argv)
{
    cxxopts::Options options("canopen_code_gen", "Generate C++ helpers from a CANopen EDS");
    options.add_options()
        ("name", "Base name for generated files", cxxopts::value<std::string>())
        ("input", "Path to the .eds", cxxopts::value<std::string>())
        ("output", "Output directory", cxxopts::value<std::string>())
        ("strict", "Treat EDS warnings as errors",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("silent", "Reduce logging",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("h,help", "Help");

    auto result = options.parse(argc, argv);
    if (result.count("help") != 0)
    {
        std::cout << options.help() << "\n";
        return 0;
    }

    const bool silent = result["silent"].as<bool>();
    const bool strict = result["strict"].as<bool>();
    if (silent)
    {
        spdlog::set_level(spdlog::level::warn);
    }

    const std::string base = result["name"].as<std::string>();
    const std::string input = result["input"].as<std::string>();
    const std::string outputDir = result["output"].as<std::string>();

    std::ifstream in(input);
    if (!in)
    {
        SPDLOG_ERROR("cannot read {}", input);
        return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto parsed = canopen::parse_eds(buffer.str());
    auto problems = canopen::validate(parsed.od);

    size_t errors = 0;
    size_t warnings = 0;
    for (const auto* list : { &parsed.diagnostics, &problems })
    {
        for (const auto& diagnostic : *list)
        {
            if (diagnostic.severity == canopen::Severity::Error)
            {
                SPDLOG_ERROR("{}: {}", input, canopen::to_string(diagnostic));
                ++errors;
            }
            else
            {
                SPDLOG_WARN("{}: {}", input, canopen::to_string(diagnostic));
                ++warnings;
            }
        }
    }

    if (errors != 0)
    {
        SPDLOG_ERROR("{}: {} error(s); refusing to generate code", input, errors);
        return 4;
    }
    if (strict && warnings != 0)
    {
        SPDLOG_ERROR("{}: {} warning(s) with --strict", input, warnings);
        return 4;
    }

    // --- derive everything -------------------------------------------------
    // Anything that cannot be derived is fatal. The point of --strict is to
    // catch a parser regression, and a regression that reintroduced the
    // fallback constants would otherwise look like a successful build.
    std::vector<std::string> derivationErrors;

    std::vector<PdoDescription> pdos;
    for (const auto& [index, object] : parsed.od.objects)
    {
        (void)object;
        if (!canopen::is_pdo_mapping_index(index))
        {
            continue;
        }
        if (auto pdo = describe_pdo(parsed.od, index, derivationErrors))
        {
            pdos.push_back(std::move(*pdo));
        }
    }

    std::map<std::string, uint32_t> cobids;
    for (const auto& pdo : pdos)
    {
        if (auto base_id = cobid_base(parsed.od, pdo.communicationIndex, derivationErrors))
        {
            cobids[pdo.name] = *base_id;
        }
    }

    if (!derivationErrors.empty())
    {
        for (const auto& message : derivationErrors)
        {
            SPDLOG_ERROR("{}: {}", input, message);
        }
        return 4;
    }

    if (pdos.empty())
    {
        SPDLOG_ERROR("{}: the file declares no PDOs; there is nothing to generate", input);
        return 4;
    }

    try
    {
        emit_helpers_header(outputDir + "/" + base + "_helpers.h", base, input, parsed.od, pdos,
                            cobids);
        emit_node_header(outputDir + "/" + base + "_node.h", base, input, pdos);
        emit_node_source(outputDir + "/" + base + "_node.cpp", base, input, pdos);

        // The helpers are header-only, but the build expects a translation unit
        // per generated library. Emitting one that includes the header at least
        // makes the header compile on its own.
        Writer helpersSource(outputDir + "/" + base + "_helpers.cpp");
        emit_banner(helpersSource, input);
        helpersSource.line("#include \"{}_helpers.h\"", base);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("{}", error.what());
        return 1;
    }

    SPDLOG_INFO("{}: generated {} PDO accessor set(s) for '{}' in {}", input, pdos.size(), base,
                outputDir);
    return 0;
}
