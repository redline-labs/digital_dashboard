// The browser as a real zenoh client: zenoh-pico over zenohd's ws/ listener.
// Built only with REDLINE_WASM_WITH_PICO; bindings.cpp is the decode half and
// knows nothing about the bus.
//
// SINGLE THREADED, BY CONFIGURATION. Z_FEATURE_MULTI_THREAD is 0 (threads under
// emcc mean SharedArrayBuffer and COOP/COEP headers), so there is no read task
// and no lease task: nothing happens unless JS calls busPump().
//
// EVERY bus* EXPORT MAY RETURN A PROMISE. Pico's emscripten ws link sleeps
// (emscripten_sleep, under ASYNCIFY) on connect and while waiting for data to
// read, and an export that sleeps hands JS a Promise instead of its value. Await each call and never start one while another is in flight:
// ASYNCIFY has one stack to rewind, and a second call that also sleeps corrupts
// it -- observed as "memory access out of bounds". The decode exports never
// sleep and are safe to call at any time.
//
// Samples are queued rather than delivered by callback into JS: a pico callback
// runs inside zp_read(), and re-entering JS from there would re-enter the wasm
// heap while pico still holds it. busTake() drains the queue afterwards. Health
// is the exception: busWatchHealth() feeds node_health::HealthTable inside the
// callback, and busHealthJson() reads the classified result.

#include "node_health/health_json.h"
#include "node_health/table.h"
#include "pub_sub/capnp_encoding.h"
#include "pub_sub/node_key.h"

#include <zenoh-pico.h>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace
{

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
    // Fed straight from the subscriber callbacks rather than through the queue:
    // it is the same table HealthMonitor keeps natively, so the page renders a
    // verdict computed by the node's own code.
    node_health::HealthTable health;
    bool watching_health { false };
};

Bus& bus()
{
    static Bus instance;
    return instance;
}

// Bounded: a browser that stops pumping must not be able to grow the heap
// without limit while an install or a busy bus keeps publishing.
constexpr std::size_t kMaxQueued = 4096;

// Batches read per busPump(). A read with nothing waiting blocks for the link
// timeout (Z_CONFIG_SOCKET_TIMEOUT, 100 ms), so draining what is already queued
// in one pump is what keeps throughput from being one batch per 100 ms. Bounded
// so a flood cannot hold one pump forever.
constexpr int kMaxReadsPerPump = 64;

// Pico renders an encoding id it has no name for as an EMPTY prefix plus the
// separator, so zenoh-c's "application/capnp;NodeHealth" arrives here as
// ";application/capnp;NodeHealth" -- and pub_sub::schemaNameFromEncoding()
// would then return "application/capnp;NodeHealth" as the schema name.
std::string encodingString(const z_loaned_encoding_t* encoding)
{
    std::string out;
    z_owned_string_t text;
    if (z_encoding_to_string(encoding, &text) == _Z_RES_OK)
    {
        out.assign(z_string_data(z_loan(text)), z_string_len(z_loan(text)));
        z_drop(z_move(text));
    }
    if (!out.empty() && out.front() == ';')
    {
        out.erase(0, 1);
    }
    return out;
}

std::string keyOf(const z_loaned_sample_t* sample)
{
    z_view_string_t key;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
    return std::string(z_string_data(z_loan(key)), z_string_len(z_loan(key)));
}

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

    record.keyexpr = keyOf(sample);

    record.encoding = encodingString(z_sample_encoding(sample));

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

void onHealthSample(z_loaned_sample_t* sample, void*)
{
    std::vector<std::uint8_t> payload;
    z_owned_slice_t slice;
    if (z_bytes_to_slice(z_sample_payload(sample), &slice) != _Z_RES_OK)
    {
        return;
    }
    const std::uint8_t* data = z_slice_data(z_loan(slice));
    payload.assign(data, data + z_slice_len(z_loan(slice)));
    z_drop(z_move(slice));

    const std::string encoding = encodingString(z_sample_encoding(sample));
    // No origin zid: every reporter in the tree puts its own in the sample,
    // and pico's source info is not enabled in this build.
    bus().health.sample(pub_sub::schemaNameFromEncoding(encoding), payload, "",
                        node_health::Clock::now());
}

void onNodeIdentity(z_loaned_sample_t* sample, void*)
{
    std::string zid;
    std::string name;
    if (!pub_sub::parseNodeKey(keyOf(sample), zid, name))
    {
        return;
    }
    bus().health.identity(zid, name, z_sample_kind(sample) == Z_SAMPLE_KIND_PUT,
                          node_health::Clock::now());
}

// locator is zenoh's spelling of a ws endpoint, e.g. "ws/10.0.0.93:7446".
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

using SampleHandler = void (*)(z_loaned_sample_t*, void*);

bool subscribe(const std::string& keyexpr, bool liveliness, SampleHandler handler)
{
    Bus& b = bus();
    if (!b.open) { return false; }

    z_view_keyexpr_t key;
    if (z_view_keyexpr_from_str(&key, keyexpr.c_str()) != _Z_RES_OK)
    {
        return false;
    }

    z_owned_closure_sample_t closure;
    z_closure_sample(&closure, handler, nullptr, nullptr);

    z_owned_subscriber_t subscriber;
    z_result_t result = _Z_RES_OK;
    if (liveliness)
    {
        // history: zenoh has no retained messages, so without it the tokens
        // declared before this subscription -- every node already running --
        // are never reported, and only arrivals after it show up.
        z_liveliness_subscriber_options_t options;
        z_liveliness_subscriber_options_default(&options);
        options.history = true;
        result = z_liveliness_declare_subscriber(z_loan(b.session), &subscriber, z_loan(key),
                                                 z_move(closure), &options);
    }
    else
    {
        result = z_declare_subscriber(z_loan(b.session), &subscriber, z_loan(key), z_move(closure),
                                      nullptr);
    }
    if (result != _Z_RES_OK)
    {
        return false;
    }
    b.subscribers.push_back(subscriber);
    return true;
}

bool busSubscribe(const std::string& keyexpr) { return subscribe(keyexpr, false, onDataSample); }
bool busSubscribeLiveliness(const std::string& keyexpr)
{
    return subscribe(keyexpr, true, onLivelinessSample);
}

// Starts feeding the health table: nodes/*/health, as HealthReporter publishes
// it, and the @redline/node identities that tell a node that stopped
// publishing from one that was never there. Idempotent.
bool busWatchHealth()
{
    Bus& b = bus();
    if (b.watching_health) { return true; }
    if (!subscribe("nodes/*/health", false, onHealthSample) ||
        !subscribe(std::string(pub_sub::kNodeAll), true, onNodeIdentity))
    {
        return false;
    }
    b.watching_health = true;
    return true;
}

// The health document, classified now by node_health's own table. Never
// sleeps, so it is safe between pumps.
std::string busHealthJson()
{
    const node_health::HealthTable& table = bus().health;
    return node_health::healthReportJson(table.rows(node_health::Clock::now()), table.revision())
        .dump();
}

// Drives the session: reads what the router has sent and keeps the lease
// alive, neither of which happens on its own without threads. Returns how many
// samples are waiting, or -1 once the transport has failed (the router went
// away or dropped us) -- the caller should busClose() and connect again.
int busPump()
{
    Bus& b = bus();
    if (!b.open) { return -1; }
    for (int i = 0; i < kMaxReadsPerPump; ++i)
    {
        const z_result_t result = zp_read(z_loan(b.session), nullptr);
        if (result == _Z_NO_DATA_PROCESSED) { break; }
        if (result < 0) { return -1; }
    }
    if (zp_send_keep_alive(z_loan(b.session), nullptr) < 0) { return -1; }
    return static_cast<int>(b.queue.size());
}

// Drains the queue into JS. Payloads cross as Uint8Array COPIES -- a view into
// the wasm heap would dangle as soon as the record is freed -- so the caller can
// keep them and hand them straight to decodeToJson()/healthFromPayload().
emscripten::val busTake()
{
    Bus& b = bus();
    emscripten::val out = emscripten::val::array();
    const emscripten::val uint8Array = emscripten::val::global("Uint8Array");
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
        const emscripten::val view(
            emscripten::typed_memory_view(record.payload.size(), record.payload.data()));
        item.set("payload", uint8Array.new_(view));
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
    // A new session starts from nothing: identities from the old one would
    // never be told they went away.
    b.health = node_health::HealthTable();
    b.watching_health = false;
}

}  // namespace

EMSCRIPTEN_BINDINGS(redline_bus)
{
    emscripten::function("busConnect", &busConnect);
    emscripten::function("busConnected", &busConnected);
    emscripten::function("busSubscribe", &busSubscribe);
    emscripten::function("busSubscribeLiveliness", &busSubscribeLiveliness);
    emscripten::function("busWatchHealth", &busWatchHealth);
    emscripten::function("busHealthJson", &busHealthJson);
    emscripten::function("busPump", &busPump);
    emscripten::function("busTake", &busTake);
    emscripten::function("busClose", &busClose);
}
