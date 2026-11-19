#include "host/control.h"

#include <exception>

namespace sim::sys {

namespace {

agent_control::ErrorCode code_of(ControlErrorReason reason) {
    switch (reason) {
        case ControlErrorReason::BadParams:
            return agent_control::ErrorCode::kBadParams;
        case ControlErrorReason::WrongMode:
            return agent_control::ErrorCode::kWrongMode;
        case ControlErrorReason::Failed:
            return agent_control::ErrorCode::kFailed;
    }
    return agent_control::ErrorCode::kInternal;
}

}  // namespace

void ControlMethods::add(const std::string& method, std::string summary, Handler handler,
                         agent_control::MethodKind kind) {
    dispatcher_.registerMethod(
        method,
        [run = std::move(handler)](const json& params) -> agent_control::MethodResult {
            try {
                return run(params);
            } catch (const ControlError& e) {
                return std::unexpected(agent_control::AgentError{code_of(e.reason), e.what(), json::object()});
            } catch (const std::exception& e) {
                // Anything else a handler throws is a request the simulator could not carry out (a file it could not
                // read, a name it does not know), not a bug in the dispatcher.
                return std::unexpected(agent_control::AgentError{agent_control::ErrorCode::kFailed, e.what(),
                                                                 json::object()});
            }
        },
        kind, std::move(summary));
}

std::optional<double> parse_number(const nlohmann::json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string s = value.get<std::string>();
    try {
        size_t used = 0;
        if (s.size() > 1 && s[0] == '$') {
            const unsigned long long v = std::stoull(s.substr(1), &used, 16);
            return used == s.size() - 1 ? std::optional<double>(double(v)) : std::nullopt;
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            const unsigned long long v = std::stoull(s.substr(2), &used, 16);
            return used == s.size() - 2 ? std::optional<double>(double(v)) : std::nullopt;
        }
        const double d = std::stod(s, &used);
        if (used == s.size()) {
            return d;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

namespace {

std::optional<double> number_of(const json& params, const std::string& name) {
    if (!params.is_object() || !params.contains(name) || params[name].is_null()) {
        return std::nullopt;
    }
    const auto v = parse_number(params[name]);
    if (!v) {
        throw ControlError(ControlErrorReason::BadParams, "parameter '" + name + "': expected a number");
    }
    return v;
}

}  // namespace

uint64_t param_unsigned(const json& params, const std::string& name) {
    const auto v = optional_unsigned(params, name);
    if (!v) {
        throw ControlError(ControlErrorReason::BadParams, "missing parameter '" + name + "'");
    }
    return *v;
}

std::optional<uint64_t> optional_unsigned(const json& params, const std::string& name) {
    const auto v = number_of(params, name);
    if (!v) {
        return std::nullopt;
    }
    if (*v < 0) {
        throw ControlError(ControlErrorReason::BadParams, "parameter '" + name + "': expected >= 0");
    }
    return uint64_t(*v);
}

double param_real(const json& params, const std::string& name) {
    const auto v = number_of(params, name);
    if (!v) {
        throw ControlError(ControlErrorReason::BadParams, "missing parameter '" + name + "'");
    }
    return *v;
}

std::optional<double> optional_real(const json& params, const std::string& name) {
    return number_of(params, name);
}

std::string param_string(const json& params, const std::string& name) {
    const auto v = optional_string(params, name);
    if (!v) {
        throw ControlError(ControlErrorReason::BadParams, "missing parameter '" + name + "'");
    }
    return *v;
}

std::optional<std::string> optional_string(const json& params, const std::string& name) {
    if (!params.is_object() || !params.contains(name) || params[name].is_null()) {
        return std::nullopt;
    }
    if (!params[name].is_string()) {
        throw ControlError(ControlErrorReason::BadParams, "parameter '" + name + "': expected a string");
    }
    return params[name].get<std::string>();
}

}  // namespace sim::sys
