// GET /api/health -- what every node on the bus is doing.
//
// The page prefers to read the bus itself: the wasm module holds a zenoh-pico
// session to zenohd's ws/ listener and classifies with node_health::HealthTable,
// the same table HealthMonitor wraps here, rendering the same document through
// node_health::healthReportJson. This route is what the page falls back to when
// that cannot happen -- no redline.wasm (the image does not build it yet), no
// router, or a browser that cannot reach port 7446 -- and what it shows while
// the direct session is being retried.

#include "http_server.h"

#include "node_health/health_json.h"
#include "node_health/monitor.h"

#include <nlohmann/json.hpp>

namespace web_console
{

void registerHealthRoutes(RouteRegistrar& routes, ::node_health::HealthMonitor& monitor)
{
    routes.getReply("/api/health", [&monitor] {
        // isValid() is false when there is no bus session at all -- on a board
        // that means zenoh never came up, which is worth saying plainly rather
        // than rendering as "no nodes".
        if (!monitor.isValid())
        {
            const nlohmann::json out{
                {"bus_available", false},
                {"error", "no zenoh session; nothing can be observed"},
                {"nodes", nlohmann::json::array()},
            };
            return jsonReply(200, out);
        }
        return jsonReply(200, node_health::healthReportJson(monitor.snapshot(), monitor.revision()));
    });
}

}  // namespace web_console
