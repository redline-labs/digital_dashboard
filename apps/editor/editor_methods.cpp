#include "editor/editor_methods.h"

#include "agent_control/input.h"
#include "agent_control/locator.h"
#include "dashboard/widget_identity.h"
#include "editor/canvas.h"
#include "editor/editor_window.h"
#include "editor/selection_frame.h"
#include "dashboard/widget_registry.h"

#include <QPointF>

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace editor::agent
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

std::expected<widget_type_t, AgentError> parseType(const json& params)
{
    if (!params.contains("type") || !params["type"].is_string())
    {
        return std::unexpected(badParams("'type' is required, e.g. 'carplay'."));
    }
    if (const auto type = reflection::enum_traits<widget_type_t>::try_from_string(
            params["type"].get<std::string>()))
    {
        return *type;
    }

    // Offering the valid set matters: the type names come from an enum the
    // caller cannot see, so a rejection without them is a dead end.
    json known = json::array();
#define KNOWN_CASE(enum_name, widget_class) \
    known.push_back(std::string(reflection::enum_to_string(widget_class::kWidgetType)));
    DASHBOARD_WIDGET_TABLE(KNOWN_CASE)
#undef KNOWN_CASE
    json data = json::object();
    data["known_types"] = std::move(known);
    return std::unexpected(AgentError{
        ErrorCode::kBadParams,
        "Unknown widget type '" + params["type"].get<std::string>() + "'.",
        std::move(data)});
}

json describeWindows(const Canvas& canvas)
{
    json windows = json::array();
    const auto summaries = canvas.windowSummaries();
    for (std::size_t i = 0; i < summaries.size(); ++i)
    {
        json entry = json::object();
        entry["index"] = i;
        entry["name"] = summaries[i].name;
        entry["display"] = std::string(reflection::enum_to_string(summaries[i].display));
        entry["scale"] = std::string(reflection::enum_to_string(summaries[i].scale));
        entry["size"] = json::array({summaries[i].width, summaries[i].height});
        entry["active"] = i == canvas.activeWindow();
        windows.push_back(std::move(entry));
    }
    json out = json::object();
    out["windows"] = std::move(windows);
    out["active"] = canvas.activeWindow();
    return out;
}

// A window named by index or by name, the two ways a caller holding editor.windows
// output can refer to one.
std::expected<std::size_t, AgentError> windowIndexOf(const Canvas& canvas, const json& params)
{
    if (!params.contains("window"))
    {
        return std::unexpected(badParams("'window' is required: an index or a window name."));
    }
    const json& window = params["window"];
    if (window.is_number_integer())
    {
        const auto index = window.get<long long>();
        if (index < 0 || static_cast<std::size_t>(index) >= canvas.windowCount())
        {
            return std::unexpected(badParams("No window at index " + std::to_string(index) + "; there are " +
                                             std::to_string(canvas.windowCount()) + "."));
        }
        return static_cast<std::size_t>(index);
    }
    if (window.is_string())
    {
        const auto summaries = canvas.windowSummaries();
        for (std::size_t i = 0; i < summaries.size(); ++i)
        {
            if (summaries[i].name == window.get<std::string>())
            {
                return i;
            }
        }
        json data = describeWindows(canvas);
        return std::unexpected(AgentError{ErrorCode::kBadParams,
                                          "No window named '" + window.get<std::string>() + "'.",
                                          std::move(data)});
    }
    return std::unexpected(badParams("'window' must be an index or a window name."));
}

json describeFrame(const SelectionFrame* frame)
{
    json out = json::object();
    out["id"] = frame->objectName().toStdString();
    out["type"] = std::string(reflection::enum_to_string(frame->type()));
    // Relative to the stack for a widget on a page, as the config stores it.
    out["rect"] = json::array({frame->x(), frame->y(), frame->width(), frame->height()});
    out["selected"] = frame->isSelected();
    if (const SelectionFrame* stack = frame->containerFrame())
    {
        out["container"] = stack->objectName().toStdString();
        if (const auto slot = stack->locateChild(frame))
        {
            out["page"] = stack->pageName(slot->first);
            out["visible"] = slot->first == stack->shownPage();
        }
    }
    if (frame->isContainer())
    {
        out["pages"] = frame->pageCount();
        out["shown_page"] = frame->shownPage() < frame->pageCount() ? frame->pageName(frame->shownPage()) : "";
    }
    return out;
}

json describePages(const SelectionFrame* stack)
{
    json pages = json::array();
    for (std::size_t p = 0; p < stack->pageCount(); ++p)
    {
        json page = json::object();
        page["index"] = p;
        page["name"] = stack->pageName(p);
        page["in_cycle"] = stack->pageInCycle(p);
        page["shown"] = p == stack->shownPage();
        json widgets = json::array();
        for (const SelectionFrame* child : stack->pageFrames(p))
        {
            widgets.push_back(child->objectName().toStdString());
        }
        page["widgets"] = std::move(widgets);
        pages.push_back(std::move(page));
    }
    json out = json::object();
    out["stack"] = stack->objectName().toStdString();
    out["pages"] = std::move(pages);
    return out;
}

// A page of `stack` named by index or by name.
std::expected<std::size_t, AgentError> pageIndexOf(const SelectionFrame* stack, const json& params,
                                                   const char* key = "page")
{
    if (!params.contains(key))
    {
        return std::unexpected(badParams(std::string("'") + key + "' is required: a page name or index."));
    }
    const json& page = params[key];
    if (page.is_number_integer())
    {
        const auto index = page.get<long long>();
        if (index >= 0 && static_cast<std::size_t>(index) < stack->pageCount())
        {
            return static_cast<std::size_t>(index);
        }
    }
    else if (page.is_string())
    {
        if (const auto index = stack->pageIndex(page.get<std::string>()))
        {
            return *index;
        }
    }
    return std::unexpected(AgentError{ErrorCode::kBadParams, "No such page on '" + stack->objectName().toStdString() + "'.",
                                      describePages(stack)});
}

}  // namespace

void registerEditorMethods(AgentServer& server, EditorWindow& window)
{
    AgentServer* server_ptr = &server;
    EditorWindow* window_ptr = &window;

    auto canvasOf = [window_ptr]() -> std::expected<Canvas*, AgentError>
    {
        Canvas* canvas = window_ptr->canvas();
        if (canvas == nullptr)
        {
            return std::unexpected(internalError("The editor has no canvas."));
        }
        return canvas;
    };

    // Resolves a selector to the SelectionFrame wrapping it. A selector by id
    // lands on the frame itself; one that lands on the inner widget is walked up.
    auto frameOf = [server_ptr](const json& params) -> std::expected<SelectionFrame*, AgentError>
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

        for (QWidget* node = found.value(); node != nullptr; node = node->parentWidget())
        {
            if (auto* frame = qobject_cast<SelectionFrame*>(node))
            {
                return frame;
            }
        }

        json data = json::object();
        data["target"] = params["target"];
        return std::unexpected(AgentError{
            ErrorCode::kBadParams,
            "That widget is not on the canvas (no enclosing SelectionFrame).",
            std::move(data)});
    };

    // A selector that must land on a page_stack.
    auto stackOf = [frameOf](const json& params, const char* key = "target") -> std::expected<SelectionFrame*, AgentError>
    {
        json lookup = json::object();
        if (!params.contains(key))
        {
            return std::unexpected(badParams(std::string("'") + key + "' is required: a page_stack selector."));
        }
        lookup["target"] = params[key];
        auto frame = frameOf(lookup);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        if (!frame.value()->isContainer())
        {
            return std::unexpected(badParams("'" + frame.value()->objectName().toStdString() + "' is not a page_stack."));
        }
        return frame.value();
    };

    // -------------------------------------------------------- editor.palette
    server.registerMethod(
        "editor.palette",
        [](const json&) -> MethodResult
        {
            json items = json::array();
#define PALETTE_CASE(enum_name, widget_class)                                                       \
    {                                                                                    \
        json entry = json::object();                                                     \
        entry["type"] = std::string(reflection::enum_to_string(widget_class::kWidgetType)); \
        entry["friendly_name"] = std::string(widget_class::kFriendlyName);                \
        items.push_back(std::move(entry));                                                \
    }
            DASHBOARD_WIDGET_TABLE(PALETTE_CASE)
#undef PALETTE_CASE
            json out = json::object();
            out["widgets"] = std::move(items);
            return out;
        });

    // ----------------------------------------------------- editor.add_widget
    server.registerMethod(
        "editor.add_widget",
        [canvasOf, stackOf, parseTypeFn = &parseType](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            auto type = parseTypeFn(params);
            if (!type.has_value())
            {
                return std::unexpected(type.error());
            }

            if (!params.contains("x") || !params.contains("y") ||
                !params["x"].is_number_integer() || !params["y"].is_number_integer())
            {
                return std::unexpected(
                    badParams("'x' and 'y' are required integers, in canvas coordinates."));
            }

            QSize size;
            if (params.contains("width") != params.contains("height"))
            {
                return std::unexpected(
                    badParams("'width' and 'height' must be given together, or both omitted "
                              "to use the widget's own size hint."));
            }
            if (params.contains("width"))
            {
                if (!params["width"].is_number_integer() || !params["height"].is_number_integer())
                {
                    return std::unexpected(badParams("'width' and 'height' must be integers."));
                }
                size = QSize(params["width"].get<int>(), params["height"].get<int>());
            }

            const QPoint pos(params["x"].get<int>(), params["y"].get<int>());
            SelectionFrame* frame = nullptr;
            if (params.contains("container") && !params["container"].is_null())
            {
                // On a page: x and y are relative to the stack.
                auto stack = stackOf(params, "container");
                if (!stack.has_value())
                {
                    return std::unexpected(stack.error());
                }
                std::size_t page = stack.value()->shownPage();
                if (params.contains("page") && !params["page"].is_null())
                {
                    auto index = pageIndexOf(stack.value(), params);
                    if (!index.has_value())
                    {
                        return std::unexpected(index.error());
                    }
                    page = index.value();
                }
                if (*type == widget_type_t::page_stack)
                {
                    return std::unexpected(badParams("A page_stack cannot be placed on another page_stack's page."));
                }
                frame = canvas.value()->addWidgetToPage(stack.value(), page, *type, pos, size);
            }
            else
            {
                frame = canvas.value()->addWidget(*type, pos, size);
            }
            if (frame == nullptr)
            {
                return std::unexpected(internalError("Failed to create the widget."));
            }

            json out = describeFrame(frame);
            out["added"] = true;
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // --------------------------------------------------------- editor.select
    server.registerMethod(
        "editor.select",
        [canvasOf, frameOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            auto frame = frameOf(params);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            canvas.value()->selectFrame(frame.value());
            return describeFrame(frame.value());
        },
        AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------- editor.move
    server.registerMethod(
        "editor.move",
        [frameOf, canvasOf](const json& params) -> MethodResult
        {
            auto frame = frameOf(params);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!params.contains("x") || !params.contains("y") ||
                !params["x"].is_number_integer() || !params["y"].is_number_integer())
            {
                return std::unexpected(badParams("'x' and 'y' are required integers."));
            }
            // Through the canvas's history, so an agent-driven move is undoable
            // in the same stack as a dragged one. add_widget and delete open
            // their own transaction inside Canvas; move and resize touch the
            // frame directly, so they open one here.
            auto canvas = canvasOf();
            const Canvas::EditTransaction tx(canvas.value_or(nullptr),
                                             Canvas::EditSource::Widget);
            frame.value()->move(params["x"].get<int>(), params["y"].get<int>());

            return describeFrame(frame.value());
        },
        AgentServer::MethodKind::kMutating);

    // --------------------------------------------------------- editor.resize
    server.registerMethod(
        "editor.resize",
        [frameOf, canvasOf](const json& params) -> MethodResult
        {
            auto frame = frameOf(params);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!params.contains("width") || !params.contains("height") ||
                !params["width"].is_number_integer() || !params["height"].is_number_integer())
            {
                return std::unexpected(
                    badParams("'width' and 'height' are required integers."));
            }
            const QSize size(params["width"].get<int>(), params["height"].get<int>());
            if (size.isEmpty())
            {
                return std::unexpected(badParams("'width' and 'height' must be positive."));
            }
            auto canvas = canvasOf();
            const Canvas::EditTransaction tx(canvas.value_or(nullptr),
                                             Canvas::EditSource::Widget);
            if (frame.value()->child())
            {
                frame.value()->child()->resize(size);
            }
            frame.value()->resize(size);

            return describeFrame(frame.value());
        },
        AgentServer::MethodKind::kMutating);

    // --------------------------------------------------------- editor.delete
    server.registerMethod(
        "editor.delete",
        [canvasOf, frameOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            auto frame = frameOf(params);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }

            json out = describeFrame(frame.value());
            if (!canvas.value()->removeFrame(frame.value()))
            {
                return std::unexpected(internalError("That frame is not on this canvas."));
            }
            out["deleted"] = true;
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // ------------------------------------------------- editor.undo / .redo
    //
    // The agent's edits go on the same stack as the GUI's, so a sequence built
    // by an agent can be stepped back the same way -- and an agent that has just
    // made a mess of a layout can put it back rather than reloading the file and
    // losing everything else it did.
    const auto historyStep = [canvasOf](bool undo) -> MethodResult
    {
        auto canvas = canvasOf();
        if (!canvas.has_value())
        {
            return std::unexpected(canvas.error());
        }

        const bool moved = undo ? canvas.value()->undo() : canvas.value()->redo();

        json out = json::object();
        out[undo ? "undone" : "redone"] = moved;
        out["can_undo"] = canvas.value()->canUndo();
        out["can_redo"] = canvas.value()->canRedo();
        out["dirty"] = canvas.value()->isDirty();
        return out;
    };

    server.registerMethod("editor.undo",
                          [historyStep](const json&) -> MethodResult { return historyStep(true); },
                          AgentServer::MethodKind::kMutating);

    server.registerMethod("editor.redo",
                          [historyStep](const json&) -> MethodResult { return historyStep(false); },
                          AgentServer::MethodKind::kMutating);

    // ---------------------------------------------------------- editor.items
    server.registerMethod("editor.items",
                          [canvasOf](const json&) -> MethodResult
                          {
                              auto canvas = canvasOf();
                              if (!canvas.has_value())
                              {
                                  return std::unexpected(canvas.error());
                              }
                              json items = json::array();
                              for (const SelectionFrame* frame : canvas.value()->allFrames())
                              {
                                  items.push_back(describeFrame(frame));
                              }
                              json out = json::object();
                              out["editor_mode"] = canvas.value()->editorMode();
                              out["window"] = canvas.value()->windowName();
                              out["scope"] = canvas.value()->scope()
                                                 ? json(canvas.value()->scope()->objectName().toStdString())
                                                 : json(nullptr);
                              out["items"] = std::move(items);
                              return out;
                          });

    // ------------------------------------------------------- editor.set_mode
    server.registerMethod(
        "editor.set_mode",
        [canvasOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            if (!params.contains("editor_mode") || !params["editor_mode"].is_boolean())
            {
                return std::unexpected(badParams(
                    "'editor_mode' must be a boolean. True shows the grid and selection "
                    "chrome and captures clicks; false passes clicks through to the live "
                    "widgets by making the frames mouse-transparent."));
            }
            canvas.value()->setEditorMode(params["editor_mode"].get<bool>());
            json out = json::object();
            out["editor_mode"] = params["editor_mode"].get<bool>();
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // --------------------------------------------------- editor.palette_drag
    server.registerMethod(
        "editor.palette_drag",
        [canvasOf, parseTypeFn = &parseType](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            auto type = parseTypeFn(params);
            if (!type.has_value())
            {
                return std::unexpected(type.error());
            }
            if (!params.contains("x") || !params.contains("y") || !params["x"].is_number() ||
                !params["y"].is_number())
            {
                return std::unexpected(badParams("'x' and 'y' are required numbers."));
            }

            // Builds the payload exactly as WidgetPalette::startDrag does -- the
            // enum's string name, under both the plain-text and custom formats --
            // and feeds it to the real drop path. This covers everything except
            // the few lines inside QDrag::exec(), which cannot be driven by
            // synthesized events at all.
            const QString type_key =
                QString::fromStdString(std::string(reflection::enum_to_string(*type)));
            const std::vector<std::pair<QString, QByteArray>> mime{
                {QStringLiteral("text/plain"), type_key.toUtf8()},
                {QStringLiteral("application/x-dashboard-widget"), type_key.toUtf8()}};

            auto result = agent_control::sendDrop(
                canvas.value(),
                QPointF(params["x"].get<double>(), params["y"].get<double>()), mime,
                Qt::CopyAction);
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }

            json out = std::move(result.value());
            out["type"] = std::string(reflection::enum_to_string(*type));
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------- editor.save / load
    server.registerMethod(
        "editor.save",
        [window_ptr](const json& params) -> MethodResult
        {
            if (!params.contains("path") || !params["path"].is_string())
            {
                return std::unexpected(badParams("'path' is required."));
            }
            const auto path = params["path"].get<std::string>();
            if (!window_ptr->saveConfigTo(QString::fromStdString(path)))
            {
                return std::unexpected(
                    internalError("Failed to save config to '" + path + "'."));
            }
            json out = json::object();
            out["saved"] = path;
            return out;
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.load",
        [window_ptr](const json& params) -> MethodResult
        {
            if (!params.contains("path") || !params["path"].is_string())
            {
                return std::unexpected(badParams("'path' is required."));
            }
            const auto path = params["path"].get<std::string>();
            if (!window_ptr->loadConfigFrom(QString::fromStdString(path)))
            {
                return std::unexpected(
                    internalError("Failed to load config from '" + path + "'."));
            }
            json out = json::object();
            out["loaded"] = path;
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // -------------------------------------------------------- editor.windows
    //
    // A config is one or more windows and the canvas shows one of them. Every
    // other editor verb acts on the window being shown.
    server.registerMethod("editor.windows",
                          [canvasOf](const json&) -> MethodResult
                          {
                              auto canvas = canvasOf();
                              if (!canvas.has_value())
                              {
                                  return std::unexpected(canvas.error());
                              }
                              return describeWindows(*canvas.value());
                          });

    server.registerMethod(
        "editor.select_window",
        [canvasOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            auto index = windowIndexOf(*canvas.value(), params);
            if (!index.has_value())
            {
                return std::unexpected(index.error());
            }
            canvas.value()->selectWindow(index.value());
            return describeWindows(*canvas.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.add_window",
        [canvasOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }

            std::string name;
            if (params.contains("name") && !params["name"].is_null())
            {
                if (!params["name"].is_string())
                {
                    return std::unexpected(badParams("'name' must be a string."));
                }
                name = params["name"].get<std::string>();
            }

            std::optional<display_role_t> display;
            if (params.contains("display") && !params["display"].is_null())
            {
                display = params["display"].is_string()
                              ? reflection::enum_traits<display_role_t>::try_from_string(
                                    params["display"].get<std::string>())
                              : std::nullopt;
                if (!display)
                {
                    return std::unexpected(badParams("'display' must be one of: " +
                                                     reflection::enum_traits<display_role_t>::known_values()));
                }
            }

            QSize size;
            const bool has_width = params.contains("width") && !params["width"].is_null();
            const bool has_height = params.contains("height") && !params["height"].is_null();
            if (has_width != has_height)
            {
                return std::unexpected(badParams("'width' and 'height' must be given together, or both omitted."));
            }
            if (has_width)
            {
                if (!params["width"].is_number_integer() || !params["height"].is_number_integer() ||
                    params["width"].get<int>() <= 0 || params["height"].get<int>() <= 0)
                {
                    return std::unexpected(badParams("'width' and 'height' must be positive integers."));
                }
                size = QSize(params["width"].get<int>(), params["height"].get<int>());
            }

            if (!canvas.value()->addWindow(name, display, size))
            {
                json data = describeWindows(*canvas.value());
                return std::unexpected(AgentError{
                    ErrorCode::kBadParams,
                    "Refused: a window with that name already exists, or its display is taken. "
                    "Each window needs its own name and its own display.",
                    std::move(data)});
            }
            return describeWindows(*canvas.value());
        },
        AgentServer::MethodKind::kMutating);

    // ---------------------------------------------------------- editor.pages
    //
    // A page_stack's pages. Every edit is one undo step on the canvas's history;
    // `page` is a name or an index throughout.
    server.registerMethod("editor.pages",
                          [stackOf](const json& params) -> MethodResult
                          {
                              auto stack = stackOf(params);
                              if (!stack.has_value())
                              {
                                  return std::unexpected(stack.error());
                              }
                              return describePages(stack.value());
                          });

    server.registerMethod(
        "editor.add_page",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto stack = stackOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!stack.has_value()) return std::unexpected(stack.error());
            std::string name;
            if (params.contains("name") && !params["name"].is_null())
            {
                if (!params["name"].is_string()) return std::unexpected(badParams("'name' must be a string."));
                name = params["name"].get<std::string>();
            }
            if (!canvas.value()->addPage(stack.value(), name))
            {
                return std::unexpected(AgentError{ErrorCode::kBadParams, "Refused: that page name is already used.",
                                                  describePages(stack.value())});
            }
            return describePages(stack.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.remove_page",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto stack = stackOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!stack.has_value()) return std::unexpected(stack.error());
            auto page = pageIndexOf(stack.value(), params);
            if (!page.has_value()) return std::unexpected(page.error());
            if (!canvas.value()->removePage(stack.value(), page.value()))
            {
                return std::unexpected(badParams("Refused: a page_stack keeps at least one page."));
            }
            return describePages(stack.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.rename_page",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto stack = stackOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!stack.has_value()) return std::unexpected(stack.error());
            auto page = pageIndexOf(stack.value(), params);
            if (!page.has_value()) return std::unexpected(page.error());
            if (!params.contains("name") || !params["name"].is_string())
            {
                return std::unexpected(badParams("'name' is required: the page's new name."));
            }
            if (!canvas.value()->renamePage(stack.value(), page.value(), params["name"].get<std::string>()))
            {
                return std::unexpected(AgentError{ErrorCode::kBadParams,
                                                  "Refused: the name is empty, unchanged, or already a page.",
                                                  describePages(stack.value())});
            }
            return describePages(stack.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.set_page",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto stack = stackOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!stack.has_value()) return std::unexpected(stack.error());
            auto page = pageIndexOf(stack.value(), params);
            if (!page.has_value()) return std::unexpected(page.error());
            if (!params.contains("in_cycle") || !params["in_cycle"].is_boolean())
            {
                return std::unexpected(badParams("'in_cycle' is required: whether next/prev stop on the page."));
            }
            canvas.value()->setPageInCycle(stack.value(), page.value(), params["in_cycle"].get<bool>());
            return describePages(stack.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.move_page",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto stack = stackOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!stack.has_value()) return std::unexpected(stack.error());
            auto page = pageIndexOf(stack.value(), params);
            if (!page.has_value()) return std::unexpected(page.error());
            if (!params.contains("index") || !params["index"].is_number_integer() || params["index"].get<long long>() < 0)
            {
                return std::unexpected(badParams("'index' is required: the page's new position, from 0."));
            }
            const auto to = static_cast<std::size_t>(params["index"].get<long long>());
            if (to >= stack.value()->pageCount())
            {
                return std::unexpected(badParams("'index' is past the last page."));
            }
            canvas.value()->movePage(stack.value(), page.value(), to);
            return describePages(stack.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.show_page",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto stack = stackOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!stack.has_value()) return std::unexpected(stack.error());
            auto page = pageIndexOf(stack.value(), params);
            if (!page.has_value()) return std::unexpected(page.error());
            canvas.value()->showPage(stack.value(), page.value());
            return describePages(stack.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.move_to_page",
        [canvasOf, frameOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            auto frame = frameOf(params);
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!frame.has_value()) return std::unexpected(frame.error());
            SelectionFrame* stack = frame.value()->containerFrame();
            if (stack == nullptr)
            {
                return std::unexpected(badParams("That widget is not on a page."));
            }
            auto page = pageIndexOf(stack, params);
            if (!page.has_value()) return std::unexpected(page.error());
            canvas.value()->moveToPage(frame.value(), page.value());
            return describeFrame(frame.value());
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.scope",
        [canvasOf, stackOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value()) return std::unexpected(canvas.error());
            if (!params.contains("target") || params["target"].is_null())
            {
                canvas.value()->exitScope();
            }
            else
            {
                auto stack = stackOf(params);
                if (!stack.has_value()) return std::unexpected(stack.error());
                canvas.value()->selectFrame(stack.value());
                canvas.value()->enterScope(stack.value());
            }
            json out = json::object();
            out["scope"] = canvas.value()->scope() ? json(canvas.value()->scope()->objectName().toStdString())
                                                   : json(nullptr);
            return out;
        },
        AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "editor.remove_window",
        [canvasOf](const json& params) -> MethodResult
        {
            auto canvas = canvasOf();
            if (!canvas.has_value())
            {
                return std::unexpected(canvas.error());
            }
            auto index = windowIndexOf(*canvas.value(), params);
            if (!index.has_value())
            {
                return std::unexpected(index.error());
            }
            if (!canvas.value()->removeWindow(index.value()))
            {
                return std::unexpected(badParams("The last window cannot be removed."));
            }
            return describeWindows(*canvas.value());
        },
        AgentServer::MethodKind::kMutating);
}

}  // namespace editor::agent
