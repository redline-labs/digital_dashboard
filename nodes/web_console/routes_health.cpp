// GET /api/health -- what every node on the bus is doing.
//
// WHY THE NODE AND NOT THE BROWSER. The plan's design has the browser as a real
// zenoh client, decoding samples in wasm. That module is built and its decode
// path is proven -- 212/212 layout fingerprints and a NodeHealth sample decoded
// byte-identically to native. What is NOT proven is zenoh-pico's emscripten
// WebSocket transport reaching a zenohd: it sends a correct upgrade and a
// correct zenoh InitSyn frame (captured on the wire), and then never receives an
// InitAck. Upstream's own emscripten CI is build-only, so there is no evidence
// that path has ever run; the one working demo rewrote pico's connect path.
//
// So health is served the way /api/system is: read here, sent as JSON. The
// classifier is node_health::HealthMonitor -- the SAME one `inspect health` and
// the Qt app use, which is the property that actually mattered. Nothing about
// the browser's view is reimplemented; only who holds the zenoh session moved.
//
// When the pico transport is proven, this route stays useful: it is what a
// browser sees before the wasm module has loaded, and on a build without pico.

#include "http_server.h"

#include "node_health/classify.h"
#include "node_health/monitor.h"
#include "node_health/state.h"

#include <nlohmann/json.hpp>

#include <string>

namespace web_console
{
namespace
{

using nlohmann::json;

json checksJson(const node_health::HealthSnapshot& snapshot)
{
    json checks = json::array();
    for (const auto& check : snapshot.checks)
    {
        checks.push_back({
            {"name", check.name},
            {"state", std::string(node_health::to_string(check.state))},
            {"detail", check.detail},
        });
    }
    return checks;
}

json rowJson(const node_health::HealthRow& row)
{
    json item{
        {"name", row.name},
        {"zid", row.zid},
        {"verdict", std::string(node_health::to_string(row.verdict))},
        // Whether a person needs to act, decided by the classifier rather than
        // by a browser guessing from the verdict string.
        {"healthy", node_health::isHealthy(row.verdict)},
        {"restarts", row.continuity.restarts},
        {"missed_samples", row.continuity.missed},
    };

    item["age_ms"] = row.age ? json(row.age->count()) : json(nullptr);

    if (row.last)
    {
        item["state"] = std::string(node_health::to_string(row.last->state));
        item["sequence"] = row.last->sequence;
        item["uptime_ms"] = row.last->uptime_ms;
        item["period_ms"] = row.last->period_ms;
        item["pid"] = row.last->pid;
        item["checks"] = checksJson(*row.last);
    }
    else
    {
        // Alive by identity but never reported: distinct from a node that is
        // gone, and the verdict already says which.
        item["state"] = nullptr;
        item["checks"] = json::array();
    }
    return item;
}

}  // namespace

void registerHealthRoutes(RouteRegistrar& routes, ::node_health::HealthMonitor& monitor)
{
    routes.getReply("/api/health", [&monitor] {
        json out;

        // isValid() is false when there is no bus session at all -- on a board
        // that means zenoh never came up, which is worth saying plainly rather
        // than rendering as "no nodes".
        out["bus_available"] = monitor.isValid();
        if (!monitor.isValid())
        {
            out["error"] = "no zenoh session; nothing can be observed";
            out["nodes"] = json::array();
            return Reply{.status = 200, .body = out.dump(), .contentType = "application/json"};
        }

        json nodes = json::array();
        for (const auto& row : monitor.snapshot())
        {
            nodes.push_back(rowJson(row));
        }
        out["nodes"] = nodes;
        out["revision"] = monitor.revision();
        return Reply{.status = 200, .body = out.dump(), .contentType = "application/json"};
    });
}

}  // namespace web_console
