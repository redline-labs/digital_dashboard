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

#ifdef REDLINE_WASM_WITH_PICO
#include <zenoh-pico.h>

#include "node_health/codec.h"
#include "node_health/state.h"

#include <nlohmann/json.hpp>
#endif

#include <cstdint>
#include <deque>
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

#ifdef REDLINE_WASM_WITH_PICO

// The browser as a real zenoh client.
//
// SINGLE THREADED, BY CONFIGURATION. The wasm build sets Z_FEATURE_MULTI_THREAD 0
// (see the generated zenoh-pico config.h), so there is no read task and no lease
// task: nothing happens unless JS calls pump(). That is deliberate -- threads
// under emcc mean SharedArrayBuffer, which means the page needs COOP/COEP
// headers, for a module whose job is to decode.
//
// Samples are queued rather than delivered by callback into JS. A pico callback
// runs inside zp_read(), and calling back into JavaScript from there would
// re-enter the wasm heap while pico still holds it. take() drains the queue
// afterwards, on the JS side of the fence.
struct SampleRecord
{
    std::string keyexpr;
    std::string encoding;
    std::vector<std::uint8_t> payload;
    bool liveliness { false };
    bool alive { true };
};

struct Bus
{
    z_owned_session_t session;
    bool open { false };
    std::vector<z_owned_subscriber_t> subscribers;
    std::deque<SampleRecord> queue;
};

Bus& bus()
{
    static Bus instance;
    return instance;
}

// Bounded: a browser that stops pumping must not be able to grow the heap
// without limit while an install or a busy bus keeps publishing.
constexpr std::size_t kMaxQueued = 4096;

void pushSample(const z_loaned_sample_t* sample, bool liveliness)
{
    Bus& b = bus();
    if (b.queue.size() >= kMaxQueued)
    {
        b.queue.pop_front();
    }

    SampleRecord record;
    record.liveliness = liveliness;
    record.alive = z_sample_kind(sample) == Z_SAMPLE_KIND_PUT;

    z_view_string_t key;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
    record.keyexpr.assign(z_string_data(z_loan(key)), z_string_len(z_loan(key)));

    z_owned_string_t encoding;
    if (z_encoding_to_string(z_sample_encoding(sample), &encoding) == _Z_RES_OK)
    {
        record.encoding.assign(z_string_data(z_loan(encoding)), z_string_len(z_loan(encoding)));
        z_drop(z_move(encoding));
    }

    z_owned_slice_t slice;
    if (z_bytes_to_slice(z_sample_payload(sample), &slice) == _Z_RES_OK)
    {
        const std::uint8_t* data = z_slice_data(z_loan(slice));
        const std::size_t length = z_slice_len(z_loan(slice));
        record.payload.assign(data, data + length);
        z_drop(z_move(slice));
    }

    b.queue.push_back(std::move(record));
}

void onDataSample(z_loaned_sample_t* sample, void*) { pushSample(sample, false); }
void onLivelinessSample(z_loaned_sample_t* sample, void*) { pushSample(sample, true); }

// locator is a ws:// endpoint in zenoh's spelling, e.g. "ws/10.0.0.93:7446".
// A browser cannot be a peer -- no listening socket, no multicast -- so this is
// always a client.
bool busConnect(const std::string& locator)
{
    Bus& b = bus();
    if (b.open) { return true; }

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, Z_CONFIG_MODE_CLIENT);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, locator.c_str());

    if (z_open(&b.session, z_move(config), nullptr) != _Z_RES_OK)
    {
        return false;
    }
    b.open = true;
    return true;
}

bool busConnected() { return bus().open; }

bool subscribe(const std::string& keyexpr, bool liveliness)
{
    Bus& b = bus();
    if (!b.open) { return false; }

    z_view_keyexpr_t key;
    if (z_view_keyexpr_from_str(&key, keyexpr.c_str()) != _Z_RES_OK)
    {
        return false;
    }

    z_owned_closure_sample_t closure;
    z_closure_sample(&closure, liveliness ? onLivelinessSample : onDataSample, nullptr, nullptr);

    z_owned_subscriber_t subscriber;
    const z_result_t result =
        liveliness
            ? z_liveliness_declare_subscriber(z_loan(b.session), &subscriber, z_loan(key),
                                              z_move(closure), nullptr)
            : z_declare_subscriber(z_loan(b.session), &subscriber, z_loan(key), z_move(closure),
                                   nullptr);
    if (result != _Z_RES_OK)
    {
        return false;
    }
    b.subscribers.push_back(subscriber);
    return true;
}

// nodes/<name>/health -- what HealthReporter publishes.
bool busSubscribeHealth() { return subscribe("nodes/*/health", false); }

// @redline/node/<zid>/<name> -- who is on the bus at all, so a node that stops
// publishing is distinguishable from one that was never there.
bool busSubscribeNodes() { return subscribe("@redline/node/**", true); }

// Drives the session. Returns how many samples are waiting afterwards.
//
// Both calls matter: zp_read moves bytes, zp_send_keep_alive stops the router
// dropping us on lease expiry. With no threads, neither happens on its own.
int busPump()
{
    Bus& b = bus();
    if (!b.open) { return 0; }
    zp_read(z_loan(b.session), nullptr);
    zp_send_keep_alive(z_loan(b.session), nullptr);
    return static_cast<int>(b.queue.size());
}

// Drains the queue into JS. Payloads cross as Uint8Array so the caller can hand
// them straight back to decodeToJson().
emscripten::val busTake()
{
    Bus& b = bus();
    emscripten::val out = emscripten::val::array();
    int index = 0;
    while (!b.queue.empty())
    {
        const SampleRecord record = std::move(b.queue.front());
        b.queue.pop_front();

        emscripten::val item = emscripten::val::object();
        item.set("key", record.keyexpr);
        item.set("encoding", record.encoding);
        item.set("liveliness", record.liveliness);
        item.set("alive", record.alive);
        item.set("payload", emscripten::val(emscripten::typed_memory_view(
                                record.payload.size(), record.payload.data())));
        out.set(index++, item);
    }
    return out;
}

void busClose()
{
    Bus& b = bus();
    if (!b.open) { return; }
    for (auto& subscriber : b.subscribers)
    {
        z_drop(z_move(subscriber));
    }
    b.subscribers.clear();
    z_drop(z_move(b.session));
    b.open = false;
    b.queue.clear();
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

#endif

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
#ifdef REDLINE_WASM_WITH_PICO
    emscripten::function("busConnect", &busConnect);
    emscripten::function("busConnected", &busConnected);
    emscripten::function("busSubscribeHealth", &busSubscribeHealth);
    emscripten::function("busSubscribeNodes", &busSubscribeNodes);
    emscripten::function("busPump", &busPump);
    emscripten::function("busTake", &busTake);
    emscripten::function("busClose", &busClose);
    emscripten::function("healthFromPayload", &healthFromPayload);
#endif
}
