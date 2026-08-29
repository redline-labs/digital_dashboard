#include "scope_methods_detail.h"

namespace scope
{
namespace methods_detail
{

// Composition: which panels exist, and binding signals to them.
void registerPanelMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window)
{
    ScopeWindow* const win = &window;


    // ------------------------------------------------------------ composition

    registerFlushed(
        "scope.panels",
        [win](const json& /*params*/) -> MethodResult {
            json panels = json::array();
            for (const ScopeWindow::PanelEntry& entry : win->panels())
            {
                panels.push_back(describePanel(entry));
            }

            json types = json::array();
            for (const PanelTypeInfo& info : availablePanelTypes())
            {
                types.push_back(json{{"type", std::string(info.name)},
                                     {"friendly_name", std::string(info.friendly_name)}});
            }

            return json{{"panels", std::move(panels)}, {"available_types", std::move(types)}};
        });

    registerFlushed(
        "scope.add_panel",
        [win](const json& params) -> MethodResult {
            const auto type_name = params.find("type");
            if (type_name == params.end() || !type_name->is_string())
            {
                return std::unexpected(badParams("'type' (string) is required."));
            }

            const auto type = reflection::enum_traits<panel_type_t>::try_from_string(
                type_name->get<std::string>());
            if (!type || *type == panel_type_t::unknown)
            {
                AgentError error =
                    badParams("Unknown panel type '" + type_name->get<std::string>() + "'.");
                json known = json::array();
                for (const PanelTypeInfo& info : availablePanelTypes())
                {
                    known.push_back(std::string(info.name));
                }
                error.data["known_types"] = known;
                return std::unexpected(std::move(error));
            }

            QString id;
            if (const auto requested = params.find("id");
                requested != params.end() && requested->is_string())
            {
                id = QString::fromStdString(requested->get<std::string>());
            }

            const QString created = win->addPanel(*type, id);
            if (created.isEmpty())
            {
                return std::unexpected(internalError("Failed to create the panel."));
            }

            ScopeWindow::PanelEntry* entry = win->findPanel(created);
            json out = describePanel(*entry);
            out["added"] = true;
            return out;
        },
        agent_control::AgentServer::MethodKind::kMutating);

    registerFlushed(
        "scope.remove_panel",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }
            const QString id = entry.value()->id;
            return json{{"removed", win->removePanel(id)}, {"id", id.toStdString()}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------------- signals

    registerFlushed(
        "scope.browser",
        [win](const json& /*params*/) -> MethodResult {
            json candidates = json::array();
            for (const BindingCandidate& candidate : win->browser()->candidates())
            {
                candidates.push_back(candidateToJson(candidate));
            }

            return json{
                {"candidates", std::move(candidates)},
                // Said every time, because an empty list here does NOT mean the
                // bus is empty and a caller that assumes otherwise will report
                // a dead system that is merely idle.
                {"note",
                 "Topics are listed from advertisements: a publisher declares a zenoh "
                 "liveliness token when it starts, so a topic appears here whether or not it "
                 "has ever published. An empty list means no publisher is running."}};
        });

    registerFlushed(
        "scope.add_signal",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto key = params.find("zenoh_key");
            if (key == params.end() || !key->is_string())
            {
                return std::unexpected(badParams("'zenoh_key' (string) is required."));
            }

            BindingCandidate candidate;

            // Two ways in. Naming a field asks the browser what it knows about
            // it, which is how a caller avoids having to know the schema. Or
            // spell out schema and category directly, which works before any
            // scan has happened.
            const auto field = params.find("field");
            const auto schema = params.find("schema");
            if (field != params.end() && field->is_string() && schema == params.end())
            {
                if (!win->browser()->findCandidate(
                        QString::fromStdString(key->get<std::string>()),
                        QString::fromStdString(field->get<std::string>()), candidate))
                {
                    AgentError error = badParams(
                        "The browser has not seen field '" + field->get<std::string>() +
                        "' on '" + key->get<std::string>() +
                        "'. Pass 'schema' and 'type_category' explicitly, or check "
                        "scope.browser for what is advertised.");
                    return std::unexpected(std::move(error));
                }
            }
            else
            {
                if (schema == params.end() || !schema->is_string())
                {
                    return std::unexpected(
                        badParams("'schema' (string) is required when 'field' is not resolvable "
                                  "through the browser."));
                }
                candidate.zenoh_key = key->get<std::string>();
                candidate.schema_name = schema->get<std::string>();
                candidate.field_name =
                    field != params.end() && field->is_string() ? field->get<std::string>() : "";
                candidate.type_category = params.value("type_category", std::string{"float"});
                candidate.element_category = params.value("element_category", std::string{});

                // Whether the list declares a length is a fact about the SCHEMA,
                // so it is read from the schema rather than taken from the
                // caller. A caller that could assert it would be able to talk a
                // panel into accepting a binding the evaluator then refuses.
                if (const auto found = pub_sub::get_schema(candidate.schema_name))
                {
                    for (auto schema_field : found->asStruct().getFields())
                    {
                        if (candidate.field_name == schema_field.getProto().getName().cStr())
                        {
                            candidate.has_fixed_length =
                                pub_sub::fixedListLength(schema_field).has_value();
                        }
                    }
                }
            }

            // WHICH ELEMENT, for a list. Honoured whether the candidate came
            // from the browser or was constructed above, because a caller
            // naming `values` alone would otherwise always get element 0 -- and
            // the 32 rows the browser offers would be visible through
            // `scope.browser` and unreachable by this method.
            //
            // Naming an element of something that is not a list is a caller
            // error rather than something to ignore: the expression it would
            // produce is not the one asked for.
            if (const auto element = params.find("element_index"); element != params.end())
            {
                if (!element->is_number_integer() || element->get<int>() < 0)
                {
                    return std::unexpected(
                        badParams("'element_index', when given, must be a non-negative integer."));
                }
                if (!candidate.needsElementIndex())
                {
                    return std::unexpected(badParams(
                        "'element_index' was given but '" + candidate.field_name +
                        "' is not a list."));
                }
                candidate.element_index = element->get<int>();
            }

            Panel* const panel = entry.value()->panel;
            if (!panel->acceptsBinding(candidate))
            {
                AgentError error = badParams("This panel will not accept that candidate.");
                error.data["candidate"] = candidateToJson(candidate);
                return std::unexpected(std::move(error));
            }

            if (!panel->addBinding(candidate))
            {
                // Accepted in principle but declined in fact -- a duplicate, or
                // a schema not in the registry. Not an error the caller can fix
                // by retrying, so say which it was as plainly as possible.
                AgentError error =
                    badParams("The panel declined the candidate (already plotted, or its schema "
                              "is not in the registry).");
                error.data["candidate"] = candidateToJson(candidate);
                return std::unexpected(std::move(error));
            }

            return json{{"added", true}, {"panel", describePanel(*entry.value())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    registerFlushed(
        "scope.remove_signal",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto index = params.find("index");
            if (index == params.end() || !index->is_number_unsigned())
            {
                return std::unexpected(badParams("'index' (unsigned) is required."));
            }

            // Through Panel's own interface. It used to cast to
            // TimeSeriesPanel, so this answered "that panel has no removable
            // signals" for a video panel holding a stream -- a definite no about
            // a binding that was definitely there.
            if (!entry.value()->panel->removeBinding(index->get<std::size_t>()))
            {
                return std::unexpected(badParams("No signal at that index."));
            }
            return json{{"removed", true}, {"panel", describePanel(*entry.value())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // Drives the drop TARGET directly, because QDrag::exec() runs a nested event
    // loop that reads real platform events and cannot be advanced by synthesized
    // ones -- on the offscreen platform it may not run at all. Sending the
    // DragEnter -> DragMove -> Drop triple here exercises the accept/reject
    // logic and the drop handler, which is where the behaviour worth testing
    // lives; only the few lines inside exec() are left uncovered. Same
    // arrangement, and the same reasoning, as editor.palette_drag.
    registerFlushed(
        "scope.browser_drag",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto key = params.find("zenoh_key");
            const auto field = params.find("field");
            if (key == params.end() || !key->is_string() || field == params.end() ||
                !field->is_string())
            {
                return std::unexpected(
                    badParams("'zenoh_key' and 'field' (strings) are required."));
            }

            BindingCandidate candidate;
            if (!win->browser()->findCandidate(QString::fromStdString(key->get<std::string>()),
                                               QString::fromStdString(field->get<std::string>()),
                                               candidate))
            {
                return std::unexpected(badParams(
                    "The browser has not seen that field; check scope.browser for what is "
                    "advertised."));
            }

            QMimeData mime;
            mime.setData(kSignalMimeType, encodeCandidate(candidate));

            QWidget* const target = entry.value()->dock;
            const QPoint centre(target->width() / 2, target->height() / 2);

            QDragEnterEvent enter(centre, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &enter);
            const bool entered = enter.isAccepted();

            QDragMoveEvent move(centre, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &move);

            QDropEvent drop(QPointF(centre), Qt::CopyAction, &mime, Qt::LeftButton,
                            Qt::NoModifier);
            QApplication::sendEvent(target, &drop);

            return json{{"accepted", drop.isAccepted()},
                        {"drag_enter_accepted", entered},
                        {"panel", describePanel(*entry.value())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);
}

}  // namespace methods_detail
}  // namespace scope
