#include "switchboard/switchboard_methods.h"

#include "switchboard/switchboard_window.h"

namespace switchboard
{

namespace
{

using agent_control::AgentServer;
using agent_control::MethodResult;

json rowToJson(const ServiceRow& row)
{
    json out = json::object();
    out["key"] = row.key;
    out["request_schema"] = row.request_schema;
    out["response_schema"] = row.response_schema;
    out["owner_zid"] = row.owner_zid;
    out["owner"] = ServiceList::ownerLabel(row);
    out["reachable"] = row.reachable;
    out["offered_by"] = row.offered_by;
    return out;
}

std::string stringParam(const json& params, const char* name)
{
    const auto it = params.find(name);
    return (it != params.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}  // namespace

void registerSwitchboardMethods(AgentServer& server, SwitchboardWindow& window)
{
    server.registerMethod(
        "switchboard.services",
        [&window](const json&) -> MethodResult
        {
            window.serviceList().refresh();
            json rows = json::array();
            for (const ServiceRow& row : window.serviceList().rows())
            {
                rows.push_back(rowToJson(row));
            }
            json out = json::object();
            out["services"] = std::move(rows);
            return out;
        });

    server.registerMethod(
        "switchboard.select",
        [&window](const json& params) -> MethodResult
        {
            const std::string key = stringParam(params, "key");
            if (key.empty())
            {
                return std::unexpected(agent_control::badParams("'key' is required."));
            }
            std::string error;
            if (!window.selectService(key, stringParam(params, "owner_zid"), error))
            {
                return std::unexpected(agent_control::badParams(error));
            }
            json out = json::object();
            out["selected"] = rowToJson(*window.currentService());
            out["has_form"] = window.form().hasSchema();
            return out;
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "switchboard.form",
        [&window](const json&) -> MethodResult
        {
            json out = json::object();
            const auto current = window.currentService();
            out["service"] = current ? rowToJson(*current) : json(nullptr);
            out["has_form"] = window.form().hasSchema();
            out["fields"] = window.form().value();
            out["valid"] = window.form().isValid();
            out["problems"] = window.form().problems();
            out["timeout_ms"] = window.timeoutMs();
            out["call_in_flight"] = window.callInFlight();
            return out;
        });

    server.registerMethod(
        "switchboard.set_fields",
        [&window](const json& params) -> MethodResult
        {
            const auto fields = params.find("fields");
            if (fields == params.end() || !fields->is_object())
            {
                return std::unexpected(
                    agent_control::badParams("'fields' is required and must be an object."));
            }
            std::vector<std::string> errors;
            if (!window.form().setValue(*fields, errors))
            {
                agent_control::AgentError error = agent_control::badParams(
                    "The form was left unchanged: " +
                    (errors.empty() ? std::string("unknown reason") : errors.front()));
                error.data["errors"] = errors;
                return std::unexpected(std::move(error));
            }
            json out = json::object();
            out["fields"] = window.form().value();
            out["valid"] = window.form().isValid();
            out["problems"] = window.form().problems();
            return out;
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "switchboard.reset",
        [&window](const json&) -> MethodResult
        {
            window.form().resetToDefaults();
            json out = json::object();
            out["fields"] = window.form().value();
            return out;
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "switchboard.submit",
        [&window](const json& params) -> MethodResult
        {
            std::optional<int> timeout;
            if (const auto it = params.find("timeout_ms"); it != params.end())
            {
                if (!it->is_number_integer() || it->get<int>() <= 0)
                {
                    return std::unexpected(
                        agent_control::badParams("'timeout_ms' must be a positive integer."));
                }
                timeout = it->get<int>();
            }
            std::string error;
            const int call_id = window.submit(timeout, error);
            if (call_id < 0)
            {
                return std::unexpected(agent_control::badParams(error));
            }
            json out = json::object();
            out["call_id"] = call_id;
            return out;
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "switchboard.result",
        [&window](const json& params) -> MethodResult
        {
            const auto it = params.find("call_id");
            if (it == params.end() || !it->is_number_integer())
            {
                return std::unexpected(agent_control::badParams("'call_id' is required."));
            }
            const CallRecord* record = window.history().find(it->get<int>());
            if (record == nullptr)
            {
                return std::unexpected(agent_control::badParams(
                    "no such call. Ids come from switchboard.submit; only the newest " +
                    std::to_string(CallHistory::kPerKey) + " per service are kept."));
            }
            json out = recordToJson(*record);
            // What a person would read in the response pane, when this call is
            // the one it shows.
            out["status_text"] = window.response().statusText().toStdString();
            out["banner_text"] = window.response().bannerText().toStdString();
            return out;
        });

    server.registerMethod(
        "switchboard.history",
        [&window](const json& params) -> MethodResult
        {
            json calls = json::array();
            for (const CallRecord* record : window.history().forKey(stringParam(params, "key")))
            {
                calls.push_back(recordToJson(*record));
            }
            json out = json::object();
            out["calls"] = std::move(calls);
            return out;
        });
}

}  // namespace switchboard
