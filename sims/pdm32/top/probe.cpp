#include "pdm32/top/probe.h"

#include "host/debug/names.h"
#include "pdm32/top/target.h"

namespace sim::sys {

std::optional<double> read_name(Instance& instance, const std::string& name, std::string& error) {
    return sim::sys::read_name(instance.target(), name, error);
}

std::string set_name(Instance& instance, const std::string& name, const nlohmann::json& value, bool allow_memory) {
    return sim::sys::set_name(instance.target(), name, value, allow_memory);
}

}  // namespace sim::sys
