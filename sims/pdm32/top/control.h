// What the simulator's agent methods are written in: a handler returns json or throws ControlError, and the
// parameter readers accept a number as JSON or as text ("0x0100", "$0100", "256") -- addresses are read off a
// listing. Registration turns a ControlError into the agent_control reason it names, so a caller can tell a bad
// parameter (BAD_PARAMS) from a call that needs the other time mode (WRONG_MODE) from one that could not be done
// (FAILED).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "agent_control/dispatcher.h"

namespace sim::sys {

using json = nlohmann::json;

enum class ControlErrorReason : uint8_t {
    BadParams,
    WrongMode,  // the method needs the other time mode (pause in stepped mode)
    Failed,     // understood but could not be done
};

struct ControlError : std::runtime_error {
    ControlError(ControlErrorReason why, const std::string& message) : std::runtime_error(message), reason(why) {
    }
    ControlErrorReason reason;
};

// Registers the simulator's methods on an agent_control dispatcher.
class ControlMethods {
   public:
    using Handler = std::function<json(const json& params)>;

    explicit ControlMethods(agent_control::Dispatcher& dispatcher) : dispatcher_(dispatcher) {
    }

    void add(const std::string& method, std::string summary, Handler handler,
             agent_control::MethodKind kind = agent_control::MethodKind::kMutating);

   private:
    agent_control::Dispatcher& dispatcher_;
};

// Parameter readers: a missing or malformed parameter throws ControlError(BadParams) naming it.
uint64_t param_unsigned(const json& params, const std::string& name);
std::optional<uint64_t> optional_unsigned(const json& params, const std::string& name);
double param_real(const json& params, const std::string& name);
std::optional<double> optional_real(const json& params, const std::string& name);
std::string param_string(const json& params, const std::string& name);
std::optional<std::string> optional_string(const json& params, const std::string& name);

}  // namespace sim::sys
