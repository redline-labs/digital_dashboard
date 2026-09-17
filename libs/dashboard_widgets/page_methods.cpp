#include "dashboard/page_methods.h"

#include "dashboard/page_command.h"
#include "dashboard/widget_registry.h"

#include <QApplication>
#include <QWidget>

#include <string>
#include <vector>

namespace dashboard::agent
{

namespace
{

using agent_control::AgentError;
using agent_control::AgentServer;
using agent_control::badParams;
using agent_control::ErrorCode;
using agent_control::json;
using agent_control::MethodResult;

// Started stacks only: an editor preview has no pages and nothing to switch.
std::vector<PageStackWidget*> liveStacks()
{
    std::vector<PageStackWidget*> out;
    for (QWidget* top : QApplication::topLevelWidgets())
    {
        for (PageStackWidget* stack : top->findChildren<PageStackWidget*>())
        {
            if (stack->started())
            {
                out.push_back(stack);
            }
        }
    }
    return out;
}

json describe(const PageStackWidget* stack)
{
    json out = json::object();
    out["id"] = stack->id();
    out["window"] = stack->window()->objectName().toStdString();
    out["path"] = agent_control::WidgetLocator::pathOf(stack).toStdString();
    out["default_page"] = stack->getConfig().default_page;
    out["current"] = stack->currentPage();
    out["previous"] = stack->previousPage();
    out["command_key"] = dashboard::pageCommandKey(stack->id());
    out["state_key"] = dashboard::pageStateKey(stack->id());

    json pages = json::array();
    for (std::size_t p = 0; p < stack->pageCount(); ++p)
    {
        json page = json::object();
        page["name"] = stack->pageInfo(p).name;
        page["in_cycle"] = stack->pageInfo(p).in_cycle;
        page["visible"] = stack->page(p)->isVisible();
        json children = json::array();
        for (QWidget* child : stack->pageChildren(p))
        {
            children.push_back(child->objectName().toStdString());
        }
        page["children"] = std::move(children);
        pages.push_back(std::move(page));
    }
    out["pages"] = std::move(pages);

    json triggers = json::array();
    for (const PageStackWidget::TriggerInfo& info : stack->triggerInfo())
    {
        json trigger = json::object();
        trigger["zenoh_key"] = info.zenoh_key;
        trigger["valid"] = info.valid;
        trigger["primed"] = info.primed;
        trigger["fired"] = info.fired;
        triggers.push_back(std::move(trigger));
    }
    out["triggers"] = std::move(triggers);
    return out;
}

}  // namespace

void registerPageMethods(AgentServer& server)
{
    server.registerMethod("pages.list",
                          [](const json& /*params*/) -> MethodResult
                          {
                              json stacks = json::array();
                              for (PageStackWidget* stack : liveStacks())
                              {
                                  stacks.push_back(describe(stack));
                              }
                              json out = json::object();
                              out["stacks"] = std::move(stacks);
                              return out;
                          });

    server.registerMethod(
        "pages.command",
        [](const json& params) -> MethodResult
        {
            if (!params.contains("target") || !params["target"].is_string())
            {
                return std::unexpected(badParams("'target' is required: the id of a page_stack."));
            }
            if (!params.contains("action") || !params["action"].is_string())
            {
                return std::unexpected(badParams("'action' is required: next, prev, go_to or back."));
            }

            const auto action =
                reflection::enum_traits<page_action_t>::try_from_string(params["action"].get<std::string>());
            if (!action)
            {
                return std::unexpected(badParams("Unknown action '" + params["action"].get<std::string>() +
                                                 "'; expected one of: " +
                                                 reflection::enum_traits<page_action_t>::known_values()));
            }
            const std::string page = params.contains("page") && params["page"].is_string()
                                         ? params["page"].get<std::string>()
                                         : std::string();

            const std::string target = params["target"].get<std::string>();
            json known = json::array();
            for (PageStackWidget* stack : liveStacks())
            {
                known.push_back(stack->id());
                if (stack->id() != target)
                {
                    continue;
                }

                const std::string before = stack->currentPage();
                std::string why;
                if (!stack->apply(*action, page, &why))
                {
                    json data = json::object();
                    json names = json::array();
                    for (std::size_t p = 0; p < stack->pageCount(); ++p)
                    {
                        names.push_back(stack->pageInfo(p).name);
                    }
                    data["pages"] = std::move(names);
                    return std::unexpected(AgentError{ErrorCode::kBadParams, "Refused: " + why + ".", std::move(data)});
                }

                json out = json::object();
                out["current"] = stack->currentPage();
                out["previous"] = stack->previousPage();
                out["changed"] = before != stack->currentPage();
                return out;
            }

            json data = json::object();
            data["stacks"] = std::move(known);
            return std::unexpected(
                AgentError{ErrorCode::kNoSuchWidget, "No page_stack with id '" + target + "'.", std::move(data)});
        },
        AgentServer::MethodKind::kMutating);
}

}  // namespace dashboard::agent
