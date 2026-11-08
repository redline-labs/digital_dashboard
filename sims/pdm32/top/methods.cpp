#include "pdm32/top/methods.h"

#include <cmath>
#include <cstdio>
#include <sstream>

#include "hc08/disasm.h"
#include "helpers/hex.h"
#include "pdm32/top/probe.h"

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

}  // namespace

void register_methods(agent_control::Dispatcher& dispatcher, SimSession& s, ServerHooks hooks) {
    ControlMethods d(dispatcher);
    Instance& in = s.instance();
    Debugger& dbg = s.debugger();
    const bool realtime = hooks.realtime;

    // ------------------------------------------------------------------ session
    d.add("hello", "the server, firmware, engine and every method with its summary", [&dispatcher, &in](const json&) {
        return json{{"name", "pdm32_sim"},
                    {"protocol", 1},
                    {"firmware", in.cfg.firmware_path},
                    {"engine", engine_name(in.m().cfg.engine)},
                    {"symbols", in.symbols.size()},
                    {"methods", dispatcher.methodNames()}};
    });
    d.add("status", "time, pc, CPU and firmware mode, resets, CAN counters", [&s](const json&) { return s.status(); });
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
    d.add("step", "{count=1, over=false}: instructions (over: a JSR/BSR runs to its return)", [&s](const json& p) {
        return s.step(int(optional_unsigned(p, "count").value_or(1)), p.value("over", false)).to_json();
    });
    d.add("finish", "run until the current function returns", [&s](const json&) { return s.finish().to_json(); });
    d.add("reset", "power-on reset of the PDM (harness, breakpoints and watches stay)", [&s](const json&) {
        s.reset();
        return s.status();
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

    // ------------------------------------------------------------------ the connector
    d.add("outputs", "every output: pin, volts, amps, junction temp, load, firmware status",
          [&s](const json&) { return s.outputs(); });
    d.add("inputs", "every input: connector volts, open, the firmware's raw reading",
          [&s](const json&) { return s.inputs(); });
    d.add("pins", "the connector: battery, inputs and outputs", [&s, &in](const json&) {
        return json{{"battery_v", in.board.battery()}, {"inputs", s.inputs()}, {"outputs", s.outputs()}};
    });
    const auto setter = [&in, &s](const std::string& name, const json& value) {
        const std::string error = set_name(in, name, value, s.allow_write());
        if (!error.empty()) {
            throw ControlError(ControlErrorReason::BadParams, error);
        }
        return json{{"ok", true}, {"name", name}};
    };
    d.add("input.set", "{input: 1-23, volts: number | \"open\"}", [setter](const json& p) {
        return setter("in[" + std::to_string(param_unsigned(p, "input")) + "].v", p.at("volts"));
    });
    d.add("load.set", "{output: 1-32, load: \"open\" | \"short\" | \"lamp:<hot ohms>\" | ohms}",
          [setter](const json& p) {
              if (!p.contains("load")) {
                  throw ControlError(ControlErrorReason::BadParams, "missing parameter 'load'");
              }
              return setter("out[" + std::to_string(param_unsigned(p, "output")) + "].load", p.at("load"));
          });
    d.add("battery.set", "{volts}",
          [setter](const json& p) { return setter("battery", json(param_real(p, "volts"))); });
    d.add("set", "{name, value}: any settable name (in[N].v, out[N].load, battery; memory with --allow-write)",
          [setter](const json& p) { return setter(param_string(p, "name"), p.value("value", json(nullptr))); });

    // ------------------------------------------------------------------ CAN
    d.add("can.send", "{id, data: hex string, extended?}: a frame to the PDM", [&s](const json& p) {
        CanFrame f;
        f.id = uint32_t(param_unsigned(p, "id"));
        f.isExtended = p.value("extended", f.id > 0x7FF);
        if (f.id > (f.isExtended ? 0x1FFFFFFFu : 0x7FFu)) {
            throw ControlError(ControlErrorReason::BadParams, "id: does not fit the identifier");
        }
        const auto data = helpers::fromHex(optional_string(p, "data").value_or(""));
        if (!data || data->size() > 8) {
            throw ControlError(ControlErrorReason::BadParams, "data: up to 8 bytes as hex");
        }
        f.len = uint8_t(data->size());
        std::copy(data->begin(), data->end(), f.data.begin());
        s.can_send(f);
        return json{{"ok", true}};
    });
    d.add("can.recv", "{since=0, limit=100, id?}: logged frames (from_pdm, t, id, data) with their index",
          [&in](const json& p) {
              const uint64_t since = optional_unsigned(p, "since").value_or(0);
              const uint64_t limit = optional_unsigned(p, "limit").value_or(100);
              const auto id = optional_unsigned(p, "id");
              const uint64_t first = in.can_node.logged_total - in.can_node.log.size();
              json frames = json::array();
              uint64_t index = first;
              for (const auto& l : in.can_node.log) {
                  if (index++ < since) {
                      continue;
                  }
                  if (id && l.frame.id != *id) {
                      continue;
                  }
                  if (frames.size() >= limit) {
                      break;
                  }
                  frames.push_back(json{{"index", index - 1},
                                        {"t", double(l.t) / double(in.m().cfg.tick_hz)},
                                        {"from_pdm", l.from_pdm},
                                        {"id", l.frame.id},
                                        {"extended", l.frame.isExtended},
                                        {"data", hex_bytes(l.frame.data.data(), classic_payload_bytes(l.frame))}});
              }
              return json{{"next", in.can_node.logged_total}, {"frames", frames}};
          });
    d.add("signals", "{filter?}: the latest decoded DBC signals from the PDM's stream, with their age",
          [&in](const json& p) {
              const std::string filter = p.value("filter", "");
              json out = json::object();
              for (const auto& [name, v] : in.can_node.signals) {
                  if (filter.empty() || name.find(filter) != std::string::npos) {
                      out[name] =
                          json{{"value", v.value}, {"age_s", double(in.m().now() - v.t) / double(in.m().cfg.tick_hz)}};
                  }
              }
              return out;
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
    d.add("read", "{names: [...]}: values by name (t, pc, a, out[N].v, in[N].v, can:SIGNAL, SYMBOL[k], $addr, dev.REG)",
          [&in](const json& p) {
              json values = json::object();
              json errors = json::object();
              for (const std::string& n : string_list(p, "names")) {
                  std::string error;
                  const auto v = read_name(in, n, error);
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
    d.add("write", "{name, value}: like set; memory names need --allow-write",
          [setter](const json& p) { return setter(param_string(p, "name"), p.value("value", json(nullptr))); });
    d.add("names", "{filter?, limit=50}: firmware symbols", [&in](const json& p) {
        json out = json::array();
        for (const Symbol* sym : in.symbols.search(p.value("filter", ""), optional_unsigned(p, "limit").value_or(50))) {
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
    d.add("disasm", "{at (address or function; default pc), count=16}", [&in](const json& p) {
        uint16_t pc = in.m().cpu.pc;
        if (const auto at = optional_string(p, "at")) {
            if (const Symbol* sym = in.symbols.find(*at)) {
                pc = sym->address;
            } else if (const auto a = parse_number(nlohmann::json(*at))) {
                pc = uint16_t(*a);
            } else {
                throw ControlError(ControlErrorReason::BadParams, "disasm: unknown address '" + *at + "'");
            }
        } else if (const auto a = optional_unsigned(p, "at")) {
            pc = uint16_t(*a);
        }
        json out = json::array();
        const int count = int(optional_unsigned(p, "count").value_or(16));
        for (int k = 0; k < count; k++) {
            const Insn insn = decode(pc, [&in](uint16_t a) { return in.m().mem.bytes[a]; });
            out.push_back(json{{"pc", pc}, {"where", in.symbols.describe(pc)}, {"text", disassemble(insn, pc)}});
            pc = uint16_t(pc + insn.length);
        }
        return out;
    });
    d.add("memory", "{address, length=16}: bytes as hex (device registers without side effects)", [&in](const json& p) {
        const uint16_t addr = uint16_t(param_unsigned(p, "address"));
        const size_t length = std::min<uint64_t>(optional_unsigned(p, "length").value_or(16), 4096);
        std::vector<uint8_t> bytes;
        for (size_t k = 0; k < length; k++) {
            bytes.push_back(in.m().peek(uint16_t(addr + k)));
        }
        return json{{"address", addr}, {"hex", hex_bytes(bytes.data(), bytes.size())}};
    });

    // ------------------------------------------------------------------ snapshots
    d.add("snapshot", "{name}: save the whole state (machine, harness, scheduled events)", [&s](const json& p) {
        s.snapshot(param_string(p, "name"));
        return json{{"ok", true}, {"snapshots", s.snapshot_names()}};
    });
    d.add("restore", "{name}", [&s](const json& p) {
        if (!s.restore(param_string(p, "name"))) {
            throw ControlError(ControlErrorReason::Failed, "no snapshot named '" + p.value("name", "") + "'");
        }
        return s.status();
    });
    d.add("snapshots", "the saved snapshots' names", [&s](const json&) { return s.snapshot_names(); });
}

}  // namespace sim::sys
