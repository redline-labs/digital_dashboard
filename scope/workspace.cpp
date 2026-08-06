#include "scope/workspace.h"

#include "config_codec/config_validation.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <type_traits>

namespace scope
{

bool operator==(const panel_entry_t& lhs, const panel_entry_t& rhs)
{
    return lhs.type == rhs.type && lhs.id == rhs.id && lhs.config == rhs.config;
}

std::vector<config_codec::Issue> validate_workspace(const YAML::Node& root)
{
    std::vector<config_codec::Issue> issues;

    if (!root.IsMap())
    {
        issues.push_back({config_codec::Issue::Severity::error, "",
                          "The workspace file must be a mapping at the top level."});
        return issues;
    }

    const YAML::Node panels = root["panels"];
    if (panels && !panels.IsSequence())
    {
        issues.push_back({config_codec::Issue::Severity::error, "panels",
                          "'panels' must be a sequence."});
        return issues;
    }

    if (!panels)
    {
        return issues;
    }

    for (std::size_t i = 0; i < panels.size(); ++i)
    {
        const std::string path = "panels[" + std::to_string(i) + "]";
        const YAML::Node entry = panels[i];

        if (!entry.IsMap())
        {
            issues.push_back(
                {config_codec::Issue::Severity::error, path, "Each panel must be a mapping."});
            continue;
        }

        if (!entry["type"])
        {
            issues.push_back({config_codec::Issue::Severity::error, path + ".type",
                              "A panel needs a 'type'."});
            continue;
        }

        const std::string type_name = entry["type"].as<std::string>();
        const auto type = reflection::enum_traits<panel_type_t>::try_from_string(type_name);
        if (!type || *type == panel_type_t::unknown)
        {
            // A warning, not an error: a workspace written by a newer build may
            // name a panel type this one has never heard of, and refusing to
            // load the whole file over that would make an upgrade one-way.
            std::string known;
            for (const PanelTypeInfo& info : availablePanelTypes())
            {
                known += (known.empty() ? "" : ", ") + std::string(info.name);
            }
            issues.push_back({config_codec::Issue::Severity::warning, path + ".type",
                              "Unknown panel type '" + type_name + "'; this panel will be "
                              "skipped. Known types: " + known + "."});
            continue;
        }

        if (!entry["id"] || entry["id"].as<std::string>().empty())
        {
            // Not fatal -- the loader assigns one -- but it means the saved
            // dock arrangement cannot be matched to this panel, because
            // restoreState() identifies docks by objectName and nothing else.
            issues.push_back({config_codec::Issue::Severity::warning, path + ".id",
                              "A panel with no 'id' cannot be matched to the saved dock "
                              "arrangement, so it will be placed by default."});
        }
    }

    return issues;
}

std::optional<scope_workspace_t> load_workspace(const std::string& path)
{
    try
    {
        const YAML::Node root = YAML::LoadFile(path);

        // Validate before decoding, same reasoning as the dashboard's loader:
        // the decoder is driven by each struct's fields, so anything the
        // structs do not claim is invisible to it. Report everything at once,
        // with paths, rather than aborting on whichever problem yaml-cpp throws
        // at first.
        bool fatal = false;
        for (const config_codec::Issue& issue : validate_workspace(root))
        {
            const std::string where = issue.path.empty() ? path : path + ": " + issue.path;
            if (issue.severity == config_codec::Issue::Severity::error)
            {
                fatal = true;
                SPDLOG_ERROR("{}: {}", where, issue.message);
            }
            else
            {
                SPDLOG_WARN("{}: {}", where, issue.message);
            }
        }

        if (fatal)
        {
            SPDLOG_CRITICAL("Refusing to load '{}': see the errors above.", path);
            return std::nullopt;
        }

        return root.as<scope_workspace_t>();
    }
    catch (const YAML::BadFile&)
    {
        SPDLOG_ERROR("Could not open workspace '{}'.", path);
        return std::nullopt;
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("Could not parse workspace '{}': {}", path, e.what());
        return std::nullopt;
    }
}

bool save_workspace(const scope_workspace_t& workspace, const std::string& path)
{
    try
    {
        YAML::Emitter emitter;
        emitter << YAML::convert<scope_workspace_t>::encode(workspace);

        // Checked at open AND after writing. A stream that opened fine can
        // still fail on a full disk, and a save that reports success while
        // leaving a truncated file is how a layout gets lost.
        std::ofstream out(path);
        if (!out)
        {
            SPDLOG_ERROR("Could not open '{}' for writing.", path);
            return false;
        }

        out << emitter.c_str() << "\n";
        out.close();

        if (!out)
        {
            SPDLOG_ERROR("Failed while writing '{}'.", path);
            return false;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to save workspace '{}': {}", path, e.what());
        return false;
    }
}

}  // namespace scope

namespace YAML
{

Node convert<scope::panel_entry_t>::encode(const scope::panel_entry_t& rhs)
{
    Node node;

    // Omitted when unset, so a workspace saved by the app stays byte-identical
    // to a hand-written one that never had the field.
    if (!rhs.id.empty())
    {
        node["id"] = rhs.id;
    }

    node["type"] = reflection::enum_to_string(rhs.type);

    std::visit(
        [&](const auto& cfg) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(cfg)>, std::monostate>)
            {
                node["config"] = cfg;
            }
        },
        rhs.config);

    return node;
}

bool convert<scope::panel_entry_t>::decode(const Node& node, scope::panel_entry_t& rhs)
{
    if (!node.IsMap())
    {
        return false;
    }

    if (node["id"])
    {
        rhs.id = node["id"].as<std::string>();
    }

    const std::string type_name = node["type"].as<std::string>();

    bool matched = false;

    // A missing `config:` is legal and means "every default". The dashboard's
    // decoder read it unconditionally and yaml-cpp throws on an undefined node,
    // so the exception escaped and failed the whole file -- for a config its own
    // validator had passed. A config the validator accepts must be one the
    // decoder accepts, or the validation is theatre.
    //
    // Default-CONSTRUCT rather than leave the variant on monostate: monostate
    // has exactly one meaning downstream -- "unknown panel type, construct
    // nothing" -- so parking a known type on it would silently drop the panel.
#define SCOPE_DECODE_PANEL_IF(enum_name, panel_class)                                          \
    if (!matched &&                                                                          \
        type_name == reflection::enum_to_string(scope::panel_class::kPanelType))             \
    {                                                                                        \
        rhs.type = scope::panel_class::kPanelType;                                           \
        rhs.config = node["config"]                                                          \
                         ? node["config"].as<typename scope::panel_class::config_t>()        \
                         : typename scope::panel_class::config_t{};                          \
        matched = true;                                                                      \
    }

    SCOPE_PANEL_TABLE(SCOPE_DECODE_PANEL_IF)
#undef SCOPE_DECODE_PANEL_IF

    if (!matched)
    {
        // Already reported by validate_workspace with its path; this is the
        // decoder's own record of what it did.
        SPDLOG_WARN("Unknown panel type '{}', skipping its configuration.", type_name);
        rhs.type = scope::panel_type_t::unknown;
    }

    return true;
}

}  // namespace YAML
