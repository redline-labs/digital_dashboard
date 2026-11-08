#include "pdm32/top/scenario.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "pdm32/top/probe.h"

namespace sim::sys {

namespace {

const char* kOps[] = {"eq", "ne", "lt", "le", "gt", "ge"};

bool compare(const Expectation& e, double value) {
    double v = value;
    if (e.mask) {
        v = double(uint32_t(std::llround(value)) & *e.mask);
    }
    if (e.op == "eq") {
        return std::fabs(v - e.operand) <= e.tolerance;
    }
    if (e.op == "ne") {
        return std::fabs(v - e.operand) > e.tolerance;
    }
    if (e.op == "lt") {
        return v < e.operand;
    }
    if (e.op == "le") {
        return v <= e.operand;
    }
    if (e.op == "gt") {
        return v > e.operand;
    }
    return v >= e.operand;
}

std::string describe(const Expectation& e) {
    char text[256];
    std::string cond = e.name;
    if (e.mask) {
        char m[32];
        std::snprintf(m, sizeof(m), " & 0x%X", *e.mask);
        cond += m;
    }
    std::snprintf(text, sizeof(text), "%s %s %g", cond.c_str(), e.op.c_str(), e.operand);
    std::string when;
    char w[96];
    if (e.at) {
        std::snprintf(w, sizeof(w), "at t=%.3f s", *e.at);
    } else {
        std::snprintf(w, sizeof(w), "by t=%.3f s (from %.3f s)", *e.by, e.after);
    }
    return std::string(text) + " " + w;
}

std::string format_value(const std::optional<double>& v) {
    if (!v) {
        return "-";
    }
    char text[32];
    if (std::isinf(*v)) {
        return "inf";
    }
    if (*v == std::floor(*v) && std::fabs(*v) < 1e9) {
        std::snprintf(text, sizeof(text), "%.0f", *v);
    } else {
        std::snprintf(text, sizeof(text), "%.3f", *v);
    }
    return text;
}

}  // namespace

bool ScenarioResult::passed() const {
    return std::all_of(expectations.begin(), expectations.end(), [](const ExpectationResult& r) { return r.passed; });
}

Scenario parse_scenario(const nlohmann::json& j, const std::string& origin) {
    const auto fail = [&](const std::string& where, const std::string& why) {
        return std::runtime_error(origin + ": " + where + ": " + why);
    };

    Scenario s;
    s.path = origin;
    s.name = j.value("name", std::filesystem::path(origin).stem().string());
    s.comment = j.value("comment", "");
    s.seconds = j.value("seconds", 1.0);
    if (j.contains("engine")) {
        s.engine = parse_engine(j.at("engine").get<std::string>());
        if (!s.engine) {
            throw fail("engine", "expected interp, block or jit");
        }
    }
    s.battery_v = j.value("battery_v", 13.5);
    s.config = j.value("config", nlohmann::json::object());

    for (size_t k = 0; k < j.value("loads", nlohmann::json::array()).size(); k++) {
        const auto& l = j.at("loads")[k];
        const std::string text =
            l.at("load").is_string() ? l.at("load").get<std::string>() : std::to_string(l.at("load").get<double>());
        s.loads.emplace_back(l.at("output").get<int>(), text);
    }
    for (const auto& i : j.value("inputs", nlohmann::json::array())) {
        s.inputs.emplace_back(i.at("input").get<int>(), i.at("volts"));
    }
    for (size_t k = 0; k < j.value("events", nlohmann::json::array()).size(); k++) {
        const auto& e = j.at("events")[k];
        if (!e.contains("t") || !e.contains("set") || !e.contains("value")) {
            throw fail("events[" + std::to_string(k) + "]", "needs t, set and value");
        }
        s.events.push_back({e.at("t").get<double>(), e.at("set").get<std::string>(), e.at("value")});
    }
    for (size_t k = 0; k < j.value("expect", nlohmann::json::array()).size(); k++) {
        const auto& e = j.at("expect")[k];
        const std::string where = "expect[" + std::to_string(k) + "]";
        Expectation x;
        x.name = e.value("read", "");
        if (x.name.empty()) {
            throw fail(where, "needs \"read\"");
        }
        if (e.contains("t")) {
            x.at = e.at("t").get<double>();
        } else if (e.contains("by")) {
            x.by = e.at("by").get<double>();
            x.after = e.value("after", 0.0);
        } else {
            throw fail(where, "needs \"t\" or \"by\"");
        }
        for (const char* op : kOps) {
            if (e.contains(op)) {
                x.op = op;
                const auto v = parse_number(e.at(op));
                if (!v) {
                    throw fail(where, std::string("bad number for ") + op);
                }
                x.operand = *v;
            }
        }
        if (x.op.empty()) {
            throw fail(where, "needs one of eq, ne, lt, le, gt, ge");
        }
        if (e.contains("mask")) {
            x.mask = uint32_t(*parse_number(e.at("mask")));
        }
        x.tolerance = e.value("tol", 0.0);
        x.why = e.value("why", "");
        s.expectations.push_back(std::move(x));
    }
    if (j.contains("trace")) {
        s.trace_period_s = j.at("trace").value("period_ms", 20.0) / 1000.0;
        s.trace_names = j.at("trace").value("names", std::vector<std::string>{});
    }
    return s;
}

Scenario load_scenario(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open scenario " + path);
    }
    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
    return parse_scenario(j, path);
}

InstanceConfig instance_config_for(const Scenario& scenario, const std::string& firmware_path) {
    InstanceConfig cfg;
    cfg.firmware_path = firmware_path;
    const std::string base = std::filesystem::path(scenario.path).parent_path().string();
    cfg.images = pdm::config::images_from_json(
        scenario.config.empty() ? nlohmann::json{{"rules", nlohmann::json::array()}} : scenario.config, base);
    if (scenario.engine) {
        cfg.engine = *scenario.engine;
    }
    return cfg;
}

void prepare_scenario(Instance& in, const Scenario& s) {
    in.harness.battery_v = s.battery_v;
    for (const auto& [output, text] : s.loads) {
        if (output < 1 || output > static_cast<int>(Pdm32Board::kOutputs)) {
            throw std::runtime_error(s.path + ": loads: output " + std::to_string(output) + " out of range");
        }
        in.harness.loads[static_cast<size_t>(output - 1)] = plant::parse_load(text);
    }
    for (const auto& [input, value] : s.inputs) {
        const std::string error = set_name(in, "in[" + std::to_string(input) + "].v", value, false);
        if (!error.empty()) {
            throw std::runtime_error(s.path + ": inputs: " + error);
        }
    }
    in.stimulus->apply();

    for (const ScenarioEvent& e : s.events) {
        Instance* instance = &in;
        const ScenarioEvent event = e;
        in.stimulus->schedule(in.ticks(e.t), e.name + " = " + e.value.dump(), [instance, event]() {
            const std::string error = set_name(*instance, event.name, event.value, true);
            if (!error.empty()) {
                throw std::runtime_error("scenario event at t=" + std::to_string(event.t) + ": " + error);
            }
        });
    }
}

ScenarioResult run_scenario(Instance& in, const Scenario& s) {
    ScenarioResult result;
    result.trace_header = {"t"};
    for (const std::string& n : s.trace_names) {
        result.trace_header.push_back(n);
    }
    for (const Expectation& e : s.expectations) {
        result.expectations.push_back({&e, false, std::nullopt, 0, ""});
    }
    std::vector<bool> decided(s.expectations.size(), false);

    const double step = 0.001;
    double next_trace = 0;
    for (double t = 0; t <= s.seconds + 1e-9; t += step) {
        in.run_until_seconds(t);
        const double now = in.seconds();

        for (size_t k = 0; k < s.expectations.size(); k++) {
            const Expectation& e = s.expectations[k];
            ExpectationResult& r = result.expectations[k];
            if (decided[k]) {
                continue;
            }
            const bool at_point = e.at && now + 1e-9 >= *e.at;
            const bool in_window = e.by && now + 1e-9 >= e.after && now <= *e.by + 1e-9;
            if (!at_point && !in_window) {
                if (e.by && now > *e.by + 1e-9) {
                    decided[k] = true;
                    r.decided_at = now;
                    r.note = "never held";
                }
                continue;
            }
            std::string error;
            r.value = read_name(in, e.name, error);
            const bool ok = r.value && compare(e, *r.value);
            if (!r.value) {
                r.note = error;
            }
            if (at_point || ok) {
                decided[k] = true;
                r.passed = ok;
                r.decided_at = now;
            }
        }

        if (!s.trace_names.empty() && now + 1e-9 >= next_trace) {
            std::vector<std::string> row;
            char t_text[32];
            std::snprintf(t_text, sizeof(t_text), "%.3f", now);
            row.push_back(t_text);
            for (const std::string& n : s.trace_names) {
                std::string error;
                row.push_back(format_value(read_name(in, n, error)));
            }
            result.trace_rows.push_back(std::move(row));
            next_trace += s.trace_period_s;
        }
    }

    for (size_t k = 0; k < s.expectations.size(); k++) {
        if (!decided[k]) {
            result.expectations[k].note = "not reached";
        }
    }
    return result;
}

std::string format_report(const Scenario& s, const ScenarioResult& r) {
    std::ostringstream out;
    out << "scenario: " << s.name << "\n";
    if (!s.comment.empty()) {
        out << "  " << s.comment << "\n";
    }

    if (!r.trace_rows.empty()) {
        // only rows where something changed, plus the first and the last
        std::vector<size_t> widths(r.trace_header.size(), 0);
        for (size_t c = 0; c < r.trace_header.size(); c++) {
            widths[c] = r.trace_header[c].size();
            for (const auto& row : r.trace_rows) {
                widths[c] = std::max(widths[c], row[c].size());
            }
        }
        const auto line = [&](const std::vector<std::string>& cells) {
            for (size_t c = 0; c < cells.size(); c++) {
                out << (c ? "  " : "") << std::string(widths[c] - cells[c].size(), ' ') << cells[c];
            }
            out << "\n";
        };
        line(r.trace_header);
        for (size_t i = 0; i < r.trace_rows.size(); i++) {
            const bool changed =
                i == 0 || i + 1 == r.trace_rows.size() ||
                !std::equal(r.trace_rows[i].begin() + 1, r.trace_rows[i].end(), r.trace_rows[i - 1].begin() + 1);
            if (changed) {
                line(r.trace_rows[i]);
            }
        }
    }

    out << "expectations:\n";
    for (const ExpectationResult& x : r.expectations) {
        char at[32];
        std::snprintf(at, sizeof(at), "%.3f", x.decided_at);
        out << "  " << (x.passed ? "PASS" : "FAIL") << "  " << describe(*x.expectation) << "  (value "
            << format_value(x.value) << " at t=" << at << ")";
        if (!x.note.empty()) {
            out << " " << x.note;
        }
        if (!x.expectation->why.empty()) {
            out << "  -- " << x.expectation->why;
        }
        out << "\n";
    }
    out << (r.passed() ? "PASSED" : "FAILED") << "\n";
    return out.str();
}

}  // namespace sim::sys
