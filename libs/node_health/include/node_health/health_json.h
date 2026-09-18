// SPDX-License-Identifier: GPL-3.0-or-later
//
// The health document the web console's page renders: HealthTable rows as
// JSON. Built into the browser's wasm module, and here rather than in wasm/ so
// it is unit-tested natively with the table it serialises.
#ifndef NODE_HEALTH_HEALTH_JSON_H_
#define NODE_HEALTH_HEALTH_JSON_H_

#include "node_health/table.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <vector>

namespace node_health
{

nlohmann::json healthRowJson(const HealthRow& row);

// {"bus_available": true, "nodes": [...], "revision": n}. A caller with no bus
// session answers {"bus_available": false, "error": ..., "nodes": []} itself.
nlohmann::json healthReportJson(const std::vector<HealthRow>& rows, std::uint64_t revision);

}  // namespace node_health

#endif  // NODE_HEALTH_HEALTH_JSON_H_
