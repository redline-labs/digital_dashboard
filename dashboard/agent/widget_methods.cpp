#include "dashboard/widget_methods.h"

#include "config_codec/config_json.h"
#include "editor/widget_registry.h"

#include <algorithm>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dashboard::agent
{

namespace
{

using agent_control::AgentError;
using agent_control::AgentServer;
using agent_control::badParams;
using agent_control::ErrorCode;
using agent_control::internalError;
using agent_control::json;
using agent_control::MethodResult;

// Which registered widget class this QWidget is, if any.
std::optional<widget_type_t> widgetTypeOf(const QWidget* widget)
{
    if (widget == nullptr)
    {
        return std::nullopt;
    }

#define TYPE_OF_CASE(enum_name, widget_class)                                    \
    if (qobject_cast<const widget_class*>(widget) != nullptr)         \
    {                                                                 \
        return widget_class::kWidgetType;                             \
    }

    DASHBOARD_WIDGET_TABLE(TYPE_OF_CASE)
#undef TYPE_OF_CASE

    return std::nullopt;
}

MethodResult configOf(QWidget* widget)
{
    const auto type = widgetTypeOf(widget);
    if (!type.has_value())
    {
        return std::unexpected(internalError("Widget is not a registered dashboard widget."));
    }

    json config;
    bool found = false;

#define GET_CONFIG_CASE(enum_name, widget_class)                                             \
    if (!found)                                                                   \
    {                                                                             \
        if (auto* typed = qobject_cast<widget_class*>(widget))                    \
        {                                                                         \
            config = dashboard::config_json::toJson(typed->getConfig());                     \
            found = true;                                                         \
        }                                                                         \
    }

    DASHBOARD_WIDGET_TABLE(GET_CONFIG_CASE)
#undef GET_CONFIG_CASE

    if (!found)
    {
        return std::unexpected(internalError("Failed to read the widget's config."));
    }

    json out = json::object();
    out["type"] = std::string(reflection::enum_to_string(*type));
    out["config"] = std::move(config);
    return out;
}

// Builds a widget_config_t whose variant holds the widget's current config with
// `patch` applied. Keeps the caller's edits partial: unmentioned fields keep
// their live values rather than reverting to type defaults.
std::expected<widget_config_t, AgentError> patchedConfig(QWidget* widget, const json& patch)
{
    const auto type = widgetTypeOf(widget);
    if (!type.has_value())
    {
        return std::unexpected(internalError("Widget is not a registered dashboard widget."));
    }

    widget_config_t out;
    out.type = *type;
    std::vector<std::string> errors;
    bool found = false;

#define PATCH_CONFIG_CASE(enum_name, widget_class)                                           \
    if (!found)                                                                   \
    {                                                                             \
        if (auto* typed = qobject_cast<widget_class*>(widget))                    \
        {                                                                         \
            auto cfg = typed->getConfig();                                        \
            dashboard::config_json::applyJson(patch, cfg, "", errors);                       \
            out.config = std::move(cfg);                                          \
            found = true;                                                         \
        }                                                                         \
    }

    DASHBOARD_WIDGET_TABLE(PATCH_CONFIG_CASE)
#undef PATCH_CONFIG_CASE

    if (!found)
    {
        return std::unexpected(internalError("Failed to read the widget's config."));
    }

    if (!errors.empty())
    {
        // Nothing is applied when any field is bad. A partially applied config
        // leaves the widget in a state neither side asked for, and the caller
        // cannot tell which half landed.
        json detail = json::array();
        for (const auto& error : errors)
        {
            detail.push_back(error);
        }
        json data = json::object();
        data["problems"] = std::move(detail);
        return std::unexpected(AgentError{ErrorCode::kBadParams,
                                          "Config was rejected; nothing was changed.",
                                          std::move(data)});
    }

    return out;
}

}  // namespace

QWidget* configBearingWidget(QWidget* target)
{
    if (target == nullptr)
    {
        return nullptr;
    }
    if (widgetTypeOf(target).has_value())
    {
        return target;
    }

    // The editor wraps every widget in a SelectionFrame, and a selector by id
    // lands on the frame. Look one level down rather than making the caller know
    // which app it is talking to.
    for (QObject* child : target->children())
    {
        if (auto* as_widget = qobject_cast<QWidget*>(child))
        {
            if (widgetTypeOf(as_widget).has_value())
            {
                return as_widget;
            }
        }
    }

    return nullptr;
}

void registerWidgetMethods(AgentServer& server, ConfigApplier applier)
{
    AgentServer* server_ptr = &server;

    auto resolve = [server_ptr](const json& params) -> std::expected<QWidget*, AgentError>
    {
        if (!params.contains("target") || !params["target"].is_string())
        {
            return std::unexpected(badParams("'target' is required."));
        }
        auto found = server_ptr->locator().resolve(params["target"].get<std::string>());
        if (!found.has_value())
        {
            return std::unexpected(found.error());
        }

        QWidget* widget = configBearingWidget(found.value());
        if (widget == nullptr)
        {
            json data = json::object();
            data["target"] = params["target"];
            data["class"] = found.value()->metaObject()->className();
            return std::unexpected(AgentError{
                ErrorCode::kBadParams,
                "That widget is not a configurable dashboard widget. Use ui.snapshot "
                "to find one (they are the non-Q* classes).",
                std::move(data)});
        }
        return widget;
    };

    // ------------------------------------------------------ widget.get_config
    server.registerMethod("widget.get_config",
                          [resolve](const json& params) -> MethodResult
                          {
                              auto widget = resolve(params);
                              if (!widget.has_value())
                              {
                                  return std::unexpected(widget.error());
                              }
                              return configOf(widget.value());
                          });

    // ------------------------------------------------- widget.describe_config
    server.registerMethod(
        "widget.describe_config",
        [resolve](const json& params) -> MethodResult
        {
            // By type name, or by target when the caller has a widget in hand
            // and does not want to look its type up first.
            std::optional<widget_type_t> type;

            if (params.contains("type"))
            {
                if (!params["type"].is_string())
                {
                    return std::unexpected(badParams("'type' must be a string."));
                }
                try
                {
                    type = reflection::enum_traits<widget_type_t>::from_string(
                        params["type"].get<std::string>());
                }
                catch (const std::exception&)
                {
                    json data = json::object();
                    json known = json::array();
#define KNOWN_TYPE_CASE(enum_name, widget_class) \
    known.push_back(std::string(reflection::enum_to_string(widget_class::kWidgetType)));
                    DASHBOARD_WIDGET_TABLE(KNOWN_TYPE_CASE)
#undef KNOWN_TYPE_CASE
                    data["known_types"] = std::move(known);
                    return std::unexpected(AgentError{
                        ErrorCode::kBadParams,
                        "Unknown widget type '" + params["type"].get<std::string>() + "'.",
                        std::move(data)});
                }
            }
            else
            {
                auto widget = resolve(params);
                if (!widget.has_value())
                {
                    return std::unexpected(widget.error());
                }
                type = widgetTypeOf(widget.value());
            }

            if (!type.has_value())
            {
                return std::unexpected(badParams("Pass either 'type' or 'target'."));
            }

            json schema;
            bool found = false;
#define DESCRIBE_CASE(enum_name, widget_class)                                                    \
    if (!found && *type == widget_class::kWidgetType)                                  \
    {                                                                                  \
        schema = dashboard::config_json::describeType<widget_class::config_t>();                   \
        schema["friendly_name"] = std::string(widget_class::kFriendlyName);             \
        found = true;                                                                   \
    }
            DASHBOARD_WIDGET_TABLE(DESCRIBE_CASE)
#undef DESCRIBE_CASE

            if (!found)
            {
                return std::unexpected(internalError("No schema for that widget type."));
            }

            json out = json::object();
            out["type"] = std::string(reflection::enum_to_string(*type));
            out["schema"] = std::move(schema);
            return out;
        });

    // ------------------------------------------------------ widget.set_config
    if (applier)
    {
        server.registerMethod(
            "widget.set_config",
            [resolve, applier](const json& params) -> MethodResult
            {
                auto widget = resolve(params);
                if (!widget.has_value())
                {
                    return std::unexpected(widget.error());
                }

                if (!params.contains("config") || !params["config"].is_object())
                {
                    return std::unexpected(badParams(
                        "'config' must be an object of the fields to change. Only the "
                        "fields you name are touched; use widget.describe_config to "
                        "see what exists."));
                }

                auto patched = patchedConfig(widget.value(), params["config"]);
                if (!patched.has_value())
                {
                    return std::unexpected(patched.error());
                }

                if (!applier(widget.value(), patched.value()))
                {
                    return std::unexpected(AgentError{
                        ErrorCode::kUnsupportedInApp,
                        "This application cannot rebuild that widget in place.",
                        json::object()});
                }

                json out = json::object();
                out["applied"] = params["config"];
                out["type"] = std::string(reflection::enum_to_string(patched.value().type));
                return out;
            },
            AgentServer::MethodKind::kMutating);
    }
}

}  // namespace dashboard::agent
