#include "host/debug/methods.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "helpers/hex.h"
#include "host/control.h"
#include "host/debug/names.h"

namespace sim::sys {

namespace {

std::vector<std::string> string_list(const json& params, const std::string& name) {
    std::vector<std::string> out;
    if (params.contains(name)) {
        if (params[name].is_string()) {
            out.push_back(params[name].get<std::string>());
        } else if (params[name].is_array()) {
            for (const auto& v : params[name]) {
                out.push_back(v.get<std::string>());
            }
        } else {
            throw ControlError(ControlErrorReason::BadParams, "parameter '" + name + "': expected a list of names");
        }
    }
    return out;
}

json value_json(const std::optional<double>& v) {
    if (!v) {
        return nullptr;
    }
    if (std::isinf(*v)) {
        return "inf";
    }
    return *v;
}

std::string hex_bytes(const uint8_t* data, size_t n) {
    return helpers::toHex(std::span<const uint8_t>(data, n), {}, helpers::HexCase::kUpper);
}

// An address parameter: a number, or text that is an address or a symbol.
uint64_t address_param(DebugTarget& t, const json& p, const std::string& key, uint64_t fallback) {
    if (!p.contains(key)) {
        return fallback;
    }
    if (p[key].is_string()) {
        const std::string text = p[key].get<std::string>();
        if (const Symbol* sym = t.symbols().find(text)) {
            return sym->address;
        }
        if (const auto a = parse_number(nlohmann::json(text))) {
            return uint64_t(*a);
        }
        throw ControlError(ControlErrorReason::BadParams, key + ": unknown address '" + text + "'");
    }
    return param_unsigned(p, key);
}

}  // namespace

void register_debug_methods(agent_control::Dispatcher& dispatcher, DebugSession& s, const ServerHooks& hooks,
                            DebugMethodsOptions o) {
    ControlMethods d(dispatcher);
    DebugTarget& t = s.target();
    Debugger& dbg = s.debugger();
    const bool realtime = hooks.realtime;
    const bool allow_write = o.allow_write;
    std::function<json()> status = o.status;
    if (!status) {
        status = [] { return json::object(); };
    }

    // ------------------------------------------------------------------ session
    d.add("hello", "the server, its CPU, firmware, engine and every method with its summary",
          [&dispatcher, &t, app = o.app, info = o.info](const json&) {
              json out{{"name", app},
                       {"protocol", 1},
                       {"cpu", t.cpu()},
                       {"symbols", t.symbols().size()},
                       {"methods", dispatcher.methodNames()}};
              if (info) {
                  out.update(info());
              }
              return out;
          });
    d.add("run", "advance time: {seconds} or {until} (s), or {us} / {until_us}; stops early at a break or watch",
          [&s, realtime](const json& p) {
              if (realtime) {
                  throw ControlError(ControlErrorReason::WrongMode, "run: realtime mode (use pause/continue)");
              }
              if (const auto until = optional_real(p, "until")) {
                  return s.run_until(*until).to_json();
              }
              if (const auto until_us = optional_real(p, "until_us")) {
                  return s.run_until(*until_us / 1e6).to_json();
              }
              if (const auto us = optional_real(p, "us")) {
                  return s.run_for(*us / 1e6).to_json();
              }
              return s.run_for(optional_real(p, "seconds").value_or(0.1)).to_json();
          });
    d.add("step", "{count=1, over=false}: instructions (over: a call runs to its return)", [&s](const json& p) {
        return s.step(int(optional_unsigned(p, "count").value_or(1)), p.value("over", false)).to_json();
    });
    d.add("finish", "run until the current function returns", [&s](const json&) { return s.finish().to_json(); });
    d.add("reset", "power-on reset (breakpoints and watches stay)", [&t, status, reset = o.reset](const json&) {
        if (reset) {
            reset();
        } else {
            t.reset();
        }
        return status();
    });
    d.add("quit", "end the server", [hooks](const json&) {
        if (hooks.quit) {
            hooks.quit();
        }
        return json{{"ok", true}};
    });
    d.add("pause", "realtime: stop the clock", [hooks](const json&) {
        if (!hooks.pause_continue) {
            throw ControlError(ControlErrorReason::WrongMode, "pause: stepped mode (time only moves in run)");
        }
        return hooks.pause_continue(true);
    });
    d.add("continue", "realtime: restart the clock", [hooks](const json&) {
        if (!hooks.pause_continue) {
            throw ControlError(ControlErrorReason::WrongMode, "continue: stepped mode");
        }
        return hooks.pause_continue(false);
    });

    // ------------------------------------------------------------------ debugger
    d.add("break", "{function | pc, condition?, action=stop|record, record?: [names], ignore?}", [&dbg](const json& p) {
        const std::string target = p.contains("function") ? param_string(p, "function") : param_string(p, "pc");
        const auto action = p.value("action", "stop") == "record" ? Debugger::Action::Record : Debugger::Action::Stop;
        try {
            const int id = dbg.add_break(target, p.value("condition", ""), action, string_list(p, "record"),
                                         int(optional_unsigned(p, "ignore").value_or(0)));
            return json{{"id", id}};
        } catch (const DebugError& e) {
            throw ControlError(ControlErrorReason::BadParams, e.what());
        }
    });
    d.add("watch", "{name, on=write|read|change, condition?, action=stop|record, record?: [names]}",
          [&dbg](const json& p) {
              const std::string on_text = p.value("on", "write");
              const auto on = on_text == "read"     ? Debugger::On::Read
                              : on_text == "change" ? Debugger::On::Change
                                                    : Debugger::On::Write;
              const auto action =
                  p.value("action", "stop") == "record" ? Debugger::Action::Record : Debugger::Action::Stop;
              try {
                  return json{{"id", dbg.add_watch(param_string(p, "name"), on, p.value("condition", ""), action,
                                                   string_list(p, "record"))}};
              } catch (const DebugError& e) {
                  throw ControlError(ControlErrorReason::BadParams, e.what());
              }
          });
    d.add("clear", "{id} or {all: true}", [&dbg](const json& p) {
        if (p.value("all", false)) {
            dbg.clear_all();
            return json{{"ok", true}};
        }
        return json{{"ok", dbg.clear(int(param_unsigned(p, "id")))}};
    });
    d.add("list", "breakpoints and watches with their hits", [&dbg](const json&) { return dbg.list(); });
    d.add("records", "{id=0 (all), since=0, limit=100}: what record actions captured", [&dbg](const json& p) {
        return dbg.records(int(optional_unsigned(p, "id").value_or(0)), optional_unsigned(p, "since").value_or(0),
                           optional_unsigned(p, "limit").value_or(100));
    });
    d.add("read", "{names: [...]}: values by name (t, registers, SYMBOL[k], SYMBOL.w/.l/.f, $addr, the device's names)",
          [&t](const json& p) {
              json values = json::object();
              json errors = json::object();
              for (const std::string& n : string_list(p, "names")) {
                  std::string error;
                  const auto v = read_name(t, n, error);
                  values[n] = value_json(v);
                  if (!v) {
                      errors[n] = error;
                  }
              }
              json out{{"values", values}};
              if (!errors.empty()) {
                  out["errors"] = errors;
              }
              return out;
          });
    const auto setter = [&t, allow_write](const std::string& name, const json& value) {
        const std::string error = set_name(t, name, value, allow_write);
        if (!error.empty()) {
            throw ControlError(ControlErrorReason::BadParams, error);
        }
        return json{{"ok", true}, {"name", name}};
    };
    d.add("set", "{name, value}: any settable name (the device's; registers and memory with --allow-write)",
          [setter](const json& p) { return setter(param_string(p, "name"), p.value("value", json(nullptr))); });
    d.add("write", "{name, value}: like set; memory and register names need --allow-write",
          [setter](const json& p) { return setter(param_string(p, "name"), p.value("value", json(nullptr))); });
    d.add("names", "{filter?, limit=50}: firmware symbols", [&t](const json& p) {
        json out = json::array();
        for (const Symbol* sym : t.symbols().search(p.value("filter", ""), optional_unsigned(p, "limit").value_or(50))) {
            out.push_back(json{{"name", sym->name},
                               {"address", sym->address},
                               {"size", sym->size},
                               {"kind", sym->kind},
                               {"comment", sym->comment}});
        }
        return out;
    });
    d.add("backtrace", "the pc and the return addresses on the stack (heuristic)",
          [&dbg](const json&) { return dbg.backtrace(); });
    d.add("registers", "every CPU register by its assembler name", [&t](const json&) { return t.registers(); });
    d.add("disasm", "{at (address or function; default pc), count=16}", [&t, &dbg](const json& p) {
        uint64_t pc = address_param(t, p, "at", t.pc());
        json out = json::array();
        const int count = int(std::min<uint64_t>(optional_unsigned(p, "count").value_or(16), 4096));
        for (int k = 0; k < count; k++) {
            const Decoded insn = t.decode(pc);
            std::vector<uint8_t> bytes(insn.length);
            const bool have = t.peek(pc, bytes.data(), bytes.size());
            out.push_back(json{{"pc", pc},
                               {"where", dbg.where(pc)},
                               {"bytes", have ? hex_bytes(bytes.data(), bytes.size()) : ""},
                               {"text", insn.text}});
            pc += insn.length;
        }
        return out;
    });
    d.add("memory", "{address, length=16}: bytes as hex (device registers without side effects)", [&t](const json& p) {
        const uint64_t addr = address_param(t, p, "address", 0);
        if (!p.contains("address")) {
            throw ControlError(ControlErrorReason::BadParams, "missing parameter 'address'");
        }
        const size_t length = std::min<uint64_t>(optional_unsigned(p, "length").value_or(16), 4096);
        std::vector<uint8_t> bytes(length);
        if (!t.peek(addr, bytes.data(), length)) {
            throw ControlError(ControlErrorReason::Failed, "memory: nothing at some of those addresses");
        }
        return json{{"address", addr}, {"hex", hex_bytes(bytes.data(), bytes.size())}};
    });

    // ------------------------------------------------------------------ snapshots
    d.add("snapshot", "{name}: save the whole state (CPU, memories, devices, the board's world)", [&s](const json& p) {
        s.snapshot(param_string(p, "name"));
        return json{{"ok", true}, {"snapshots", s.snapshot_names()}};
    });
    d.add("restore", "{name}", [&s, status](const json& p) {
        if (!s.restore(param_string(p, "name"))) {
            throw ControlError(ControlErrorReason::Failed, "no snapshot named '" + p.value("name", "") + "'");
        }
        return status();
    });
    d.add("snapshots", "the saved snapshots' names", [&s](const json&) { return s.snapshot_names(); });
}

}  // namespace sim::sys
