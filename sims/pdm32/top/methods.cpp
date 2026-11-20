#include "pdm32/top/methods.h"

#include <cmath>
#include <cstdio>
#include <sstream>

#include "helpers/hex.h"
#include "host/debug/methods.h"
#include "pdm32/top/probe.h"

namespace sim::sys {

namespace {

std::string hex_bytes(const uint8_t* data, size_t n) {
    return helpers::toHex(std::span<const uint8_t>(data, n), {}, helpers::HexCase::kUpper);
}

}  // namespace

void register_methods(agent_control::Dispatcher& dispatcher, SimSession& s, ServerHooks hooks) {
    ControlMethods d(dispatcher);
    Instance& in = s.instance();

    // ------------------------------------------------------------------ the shared debugger (host/debug/methods.h)
    DebugMethodsOptions options;
    options.app = "pdm32_sim";
    options.info = [&in] {
        return json{{"firmware", in.cfg.firmware_path}, {"engine", engine_name(in.m().cfg.engine)}};
    };
    options.status = [&s] { return s.status(); };
    options.reset = [&s] { s.reset(); };
    options.allow_write = s.allow_write();
    register_debug_methods(dispatcher, s.debug(), hooks, std::move(options));
    d.add("status", "time, pc, CPU and firmware mode, resets, CAN counters", [&s](const json&) { return s.status(); });

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
}

}  // namespace sim::sys
