// The M150 under the shared debugger (host/debug), through its agent methods as an agent would call them, on an image
// built outside the tree with its ELF files' symbols: a breakpoint on a function, registers, disassembly and a named
// backtrace, step, step over a call and finish, a data watch that stops on a write, and a snapshot that replays the
// same. Skipped without M150_IMAGE and M150_SYMBOLS (the kernel's and the test application's ELF files; the
// application: emu/ppcrun/testfw in the research tree, whose fw_main calls can_init and writes `lam` every 10 ms).
#include <cstdlib>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent_control/dispatcher.h"
#include "mctest/test.h"
#include "m150/top/served.h"

using json = nlohmann::json;

namespace {

struct Fixture {
    std::unique_ptr<sim::m150::M150Served> device;
    agent_control::Dispatcher dispatcher{"m150_sim"};
    int id = 0;

    bool open() {
        const char* image = std::getenv("M150_IMAGE");
        const char* symbols = std::getenv("M150_SYMBOLS");
        if (!image || !*image || !symbols || !*symbols) {
            return false;
        }
        sim::m150::M150Options o;
        o.image = image;
        o.symbols = sim::m150::split_paths(symbols);
        o.engine = sim::e200::Engine::Jit;
        device = std::make_unique<sim::m150::M150Served>(o);
        device->register_methods(dispatcher, sim::sys::ServerHooks{});  // stepped: run is the agent's
        return true;
    }

    json call(const std::string& method, json params = json::object()) {
        const json request{{"jsonrpc", "2.0"}, {"id", ++id}, {"method", method}, {"params", params}};
        const json reply = json::parse(dispatcher.handleLine(request.dump()));
        if (reply.contains("error")) {
            std::fprintf(stderr, "%s: %s\n", method.c_str(), reply["error"].dump().c_str());
        }
        return reply.value("result", json());
    }
};

#define NEED_IMAGE(f)                                  \
    Fixture f;                                         \
    if (!f.open()) {                                   \
        mctest::skip("M150_IMAGE or M150_SYMBOLS not set"); \
        return;                                        \
    }

}  // namespace

TEST(m150_debug_break_on_a_function_with_registers_disasm_and_backtrace) {
    NEED_IMAGE(f);
    const json hello = f.call("hello");
    CHECK(hello["cpu"] == "e200");
    CHECK(hello["symbols"].get<int>() > 100);

    const json b = f.call("break", {{"function", "fw_main"}});
    CHECK(b.contains("id"));
    const json r = f.call("run", {{"seconds", 1.0}});
    CHECK(r["reason"] == "break");
    CHECK(r["where"] == "fw_main");
    CHECK_EQ(r.value("id", -1), b.value("id", -2));

    const json regs = f.call("registers");
    CHECK_EQ(regs["pc"].get<uint64_t>(), r["pc"].get<uint64_t>());
    CHECK(regs["r1"].get<uint64_t>() >= 0x400000 && regs["r1"].get<uint64_t>() < 0x600000);  // the main stack

    const json dis = f.call("disasm", {{"count", 2}});
    CHECK(dis[0]["text"] == "stwu r1,-64(r1)");
    CHECK(dis[0]["where"] == "fw_main");
    CHECK(dis[1]["text"] == "mfspr r0,8");

    // fw_main is called from main: the backtrace names it (LR, before fw_main saves it)
    const json bt = f.call("backtrace");
    CHECK(bt.size() >= 2);
    CHECK(bt.size() >= 2 && bt[1]["where"].get<std::string>().rfind("main+", 0) == 0);
}

TEST(m150_debug_step_step_over_and_finish) {
    NEED_IMAGE(f);
    f.call("break", {{"function", "fw_main"}});
    f.call("run", {{"seconds", 1.0}});
    f.call("clear", {{"all", true}});

    // seven instructions to the call of can_init
    json s = f.call("step", {{"count", 7}});
    CHECK(s["reason"] == "step");
    CHECK(f.call("disasm", {{"count", 1}})[0]["text"].get<std::string>().rfind("b", 0) == 0);
    const uint64_t at_call = f.call("registers")["pc"];
    const uint64_t sp = f.call("registers")["r1"];
    s = f.call("step", {{"over", true}});
    CHECK_EQ(s["pc"].get<uint64_t>(), at_call + 4);
    CHECK_EQ(f.call("registers")["r1"].get<uint64_t>(), sp);

    // into the next call, and out again
    f.call("break", {{"function", "ltc_init"}});
    CHECK(f.call("run", {{"seconds", 0.1}})["where"] == "ltc_init");
    f.call("clear", {{"all", true}});
    const json fin = f.call("finish");
    CHECK(fin["reason"] == "finish");
    CHECK(fin["where"].get<std::string>().rfind("fw_main+", 0) == 0);
}

TEST(m150_debug_watch_stops_on_a_write_and_reads_a_float) {
    NEED_IMAGE(f);
    // the kernel's clearing of the bss writes `lam` too: watch it once the application runs
    f.call("break", {{"function", "fw_main"}});
    f.call("run", {{"seconds", 1.0}});
    f.call("clear", {{"all", true}});
    const json w = f.call("watch", {{"name", "lam"}, {"on", "write"}});
    CHECK(w.contains("id"));
    const json r = f.call("run", {{"seconds", 1.0}});
    CHECK(r["reason"] == "watch");
    CHECK_EQ(r.value("id", -1), w.value("id", -2));
    CHECK(r["where"].get<std::string>().rfind("fw_main+", 0) == 0);
    const json v = f.call("read", {{"names", {"lam.f", "t", "can1.bit_rate", "r1"}}});
    CHECK_EQ(v["values"]["lam.f"].get<double>(), 0.0);
    CHECK_EQ(v["values"]["can1.bit_rate"].get<double>(), 1000000.0);
    CHECK(f.call("list")[0]["hits"].get<int>() == 1);
    // and again on the next pass (10 ms on), when the page would long be in the fast path but for the watch
    const json again = f.call("run", {{"seconds", 1.0}});
    CHECK(again["reason"] == "watch");
    CHECK(again["t"].get<double>() > r["t"].get<double>() + 0.005);
    CHECK(f.call("list")[0]["hits"].get<int>() == 2);
}

TEST(m150_debug_snapshot_restore_replays_the_same) {
    NEED_IMAGE(f);
    f.call("run", {{"seconds", 0.25}});
    f.call("snapshot", {{"name", "a"}});
    f.call("run", {{"seconds", 0.25}});
    const json first = f.call("read", {{"names", {"r1", "pc", "can1.sent"}}});
    const json frames_first = f.call("can.recv", {{"limit", 1000}});
    CHECK(f.call("restore", {{"name", "a"}}).contains("pc"));
    CHECK(f.call("read", {{"names", {"can1.sent"}}})["values"]["can1.sent"].get<double>() <
          first["values"]["can1.sent"].get<double>());
    f.call("run", {{"seconds", 0.25}});
    const json again = f.call("read", {{"names", {"r1", "pc", "can1.sent"}}});
    CHECK(again["values"] == first["values"]);
    CHECK(f.call("status")["t"].get<double>() > 0.49);
}

TEST(m150_debug_memory_and_names) {
    NEED_IMAGE(f);
    const json names = f.call("names", {{"filter", "cancomms_tx_std"}});
    CHECK(names.size() >= 1 && names[0]["kind"] == "function");
    const json mem = f.call("memory", {{"address", "fw_main"}, {"length", 4}});
    CHECK(mem["hex"] == "9421FFC0");  // stwu r1,-64(r1)
    // memory and registers are not writable without --allow-write
    const json request{{"jsonrpc", "2.0"}, {"id", 99}, {"method", "write"}, {"params", {{"name", "r3"}, {"value", 1}}}};
    const json reply = json::parse(f.dispatcher.handleLine(request.dump()));
    CHECK(reply["error"]["data"]["reason"] == "BAD_PARAMS");
}
