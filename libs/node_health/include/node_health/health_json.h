// SPDX-License-Identifier: GPL-3.0-or-later
//
// The /api/health document. Here rather than in the web console so that the
// node's route and the browser's wasm module, which classifies the bus itself,
// hand the page the same shape from the same code.
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
