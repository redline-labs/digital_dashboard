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
// heap while pico still holds it. busTake() drains the queue afterwards.

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

}  // namespace

EMSCRIPTEN_BINDINGS(redline_bus)
{
    emscripten::function("busConnect", &busConnect);
    emscripten::function("busConnected", &busConnected);
    emscripten::function("busSubscribeHealth", &busSubscribeHealth);
    emscripten::function("busSubscribeNodes", &busSubscribeNodes);
    emscripten::function("busPump", &busPump);
    emscripten::function("busTake", &busTake);
    emscripten::function("busClose", &busClose);
}
