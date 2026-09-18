// The JS-facing surface of the wasm module.
//
// Everything here is a thin wrapper. The decoding, the JSON conversion and the
// layout fingerprint are the tree's own implementations, compiled for a second
// target -- that is the entire point. A browser that reimplemented the
// fingerprint in TypeScript would not fail loudly when it got a slot offset
// wrong; it would silently drop every sample of the affected schema, because
// pub_sub::detail::layoutMatches() drops mismatches rather than throwing.
//
// u64 CROSSES AS HEX, NOT AS A NUMBER. A JS number is a double, so it carries 53
// bits exactly and a layout hash is 64. Returning one as a number would compare
// equal to a different hash often enough to be useless and rarely enough to look
// fine in testing.

#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_layout.h"
#include "pub_sub/schema_registry.h"

#include <capnp/schema.h>
#include <kj/exception.h>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "node_health/codec.h"
#include "node_health/state.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

// ZERO-PADDED TO 16 DIGITS, which is not cosmetic. These strings are compared
// against the constants the registry generator baked in, and those are written
// as full-width 0x...ull literals. Unpadded, the 13 schemas whose top nibble is
// zero compare unequal to an identical value -- which is exactly the kind of
// difference that looks like a real fingerprint bug.
std::string toHex(std::uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

// kj::Exception is not a std::exception, so embind would report it as an
// unhandled foreign exception with no message. Translate at the boundary.
template <typename Fn>
auto guarded(Fn&& fn) -> decltype(fn())
{
    try
    {
        return fn();
    }
    catch (const kj::Exception& e)
    {
        throw std::runtime_error(std::string("capnp: ") + e.getDescription().cStr());
    }
}

capnp::Schema requireSchema(const std::string& name)
{
    const std::optional<capnp::Schema> schema = pub_sub::get_schema(name);
    if (!schema)
    {
        throw std::runtime_error("unknown schema: " + name);
    }
    return *schema;
}

std::vector<std::string> availableSchemas()
{
    std::vector<std::string> names;
    for (auto&& name : pub_sub::get_available_schemas())
    {
        names.emplace_back(std::string(name));
    }
    return names;
}

// The fingerprint as the REGISTRY baked it in at build time.
std::string layoutHashOf(const std::string& name)
{
    return toHex(pub_sub::schema_layout_hash(name));
}

// The fingerprint recomputed FROM THE SERIALIZED DESCRIPTOR, the way a foreign
// consumer would: load the pruned CodeGeneratorRequest through SchemaLoader and
// walk it. If this disagrees with layoutHashOf() for any schema, the descriptor
// path is broken -- which is worth knowing here rather than after a recording is
// made. This is the assertion the spike turns on.
std::string layoutHashOfDescriptor(const std::string& name)
{
    return guarded([&] {
        const std::span<const std::uint8_t> descriptor = pub_sub::schema_descriptor(name);
        const std::optional<std::uint64_t> hash = pub_sub::layoutHashOfDescriptor(descriptor, name);
        if (!hash)
        {
            throw std::runtime_error("no layout hash from descriptor for " + name);
        }
        return toHex(*hash);
    });
}

std::string decodeToJson(const std::string& name, emscripten::val bytes)
{
    return guarded([&] {
        const std::vector<std::uint8_t> payload =
            emscripten::convertJSArrayToNumberVector<std::uint8_t>(bytes);
        // data_hex_limit mirrors what switchboard passes, so a Data field shows
        // as hex rather than as an unbounded byte array in a browser console.
        const pub_sub::CapnpJsonOptions options{.data_hex_limit = 4096};
        return pub_sub::capnpToJson(payload, requireSchema(name), options).dump();
    });
}

std::string describeSchemaJson(const std::string& name)
{
    return guarded([&] { return pub_sub::describeSchema(requireSchema(name)).dump(); });
}

// Doc comments are NOT in the descriptor -- the registry plugin strips
// CodeGeneratorRequest.sourceInfo -- so they come from the generated tables,
// keyed by capnp node id. Exposed because a form built in the browser wants the
// field documentation the schema author wrote.
std::string schemaDoc(const std::string& name)
{
    return guarded(
        [&] { return std::string(pub_sub::schema_doc(requireSchema(name).getProto().getId())); });
}

std::string memberDoc(const std::string& name, std::uint32_t index)
{
    return guarded([&] {
        return std::string(pub_sub::member_doc(requireSchema(name).getProto().getId(), index));
    });
}

// The raw descriptor, for a browser that wants to drive capnp::SchemaLoader
// itself rather than go through describeSchemaJson().
emscripten::val descriptorBytes(const std::string& name)
{
    const std::span<const std::uint8_t> descriptor = pub_sub::schema_descriptor(name);
    return emscripten::val(emscripten::typed_memory_view(descriptor.size(), descriptor.data()));
}

// Decodes a NodeHealth sample with the SAME codec the cluster and `inspect` use.
// Reimplementing this in JS is what the whole wasm module exists to avoid.
std::string healthFromPayload(emscripten::val bytes)
{
    const std::vector<std::uint8_t> payload =
        emscripten::convertJSArrayToNumberVector<std::uint8_t>(bytes);

    const std::optional<node_health::HealthSnapshot> snapshot =
        node_health::decodePayload(payload);
    if (!snapshot)
    {
        return R"({"error":"not a NodeHealth sample"})";
    }

    nlohmann::json out{
        {"node", snapshot->node},
        {"zid", snapshot->zid},
        {"state", std::string(node_health::to_string(snapshot->state))},
        {"sequence", snapshot->sequence},
        {"uptime_ms", snapshot->uptime_ms},
        {"period_ms", snapshot->period_ms},
        {"pid", snapshot->pid},
    };

    nlohmann::json checks = nlohmann::json::array();
    for (const auto& check : snapshot->checks)
    {
        checks.push_back({
            {"name", check.name},
            {"state", std::string(node_health::to_string(check.state))},
            {"detail", check.detail},
        });
    }
    out["checks"] = checks;
    return out.dump();
}

}  // namespace

EMSCRIPTEN_BINDINGS(redline)
{
    emscripten::register_vector<std::string>("StringVector");

    emscripten::function("availableSchemas", &availableSchemas);
    emscripten::function("layoutHashOf", &layoutHashOf);
    emscripten::function("layoutHashOfDescriptor", &layoutHashOfDescriptor);
    emscripten::function("decodeToJson", &decodeToJson);
    emscripten::function("describeSchemaJson", &describeSchemaJson);
    emscripten::function("schemaDoc", &schemaDoc);
    emscripten::function("memberDoc", &memberDoc);
    emscripten::function("descriptorBytes", &descriptorBytes);
    emscripten::function("healthFromPayload", &healthFromPayload);
}
