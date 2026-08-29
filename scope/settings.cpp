#include "scope/settings.h"

#include <spdlog/spdlog.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>

#include <set>

namespace scope
{
namespace
{

// The fields scope_settings_t claims. Anything else in the file is reported as
// a warning by validate_settings, the same way the workspace loader does it --
// yaml-cpp's decoder is driven by the struct's fields, so a misspelt key is
// otherwise silently ignored and the setting simply never takes effect.
constexpr const char* kTilesetFields[] = {"name", "path"};

bool isKnownTilesetField(const std::string& key)
{
    for (const char* field : kTilesetFields)
    {
        if (key == field)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string settingsPath()
{
    // AppConfigLocation, not AppDataLocation: this is configuration a person
    // may reasonably open in an editor, not application state. Qt appends the
    // organization and application names, which main() sets before anything
    // gets here.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
    {
        // Only reachable with no HOME at all. Returning a path in the working
        // directory keeps every caller's contract intact -- load falls back to
        // defaults if it is not there, and save reports a real error.
        SPDLOG_WARN("No writable config location; settings will use ./scope.yaml");
        return "scope.yaml";
    }
    return (dir + "/scope.yaml").toStdString();
}

std::vector<config_codec::Issue> validate_settings(const YAML::Node& root)
{
    std::vector<config_codec::Issue> issues;

    if (!root.IsMap())
    {
        issues.push_back({config_codec::Issue::Severity::error, "",
                          "The settings file must be a mapping at the top level."});
        return issues;
    }

    const YAML::Node tilesets = root["tilesets"];
    if (tilesets && !tilesets.IsSequence())
    {
        issues.push_back({config_codec::Issue::Severity::error, "tilesets",
                          "'tilesets' must be a sequence."});
        return issues;
    }

    if (!tilesets)
    {
        return issues;
    }

    for (std::size_t i = 0; i < tilesets.size(); ++i)
    {
        const std::string path = "tilesets[" + std::to_string(i) + "]";
        const YAML::Node entry = tilesets[i];

        if (!entry.IsMap())
        {
            issues.push_back({config_codec::Issue::Severity::error, path,
                              "Each tileset must be a mapping with 'name' and 'path'."});
            continue;
        }

        for (const auto& field : entry)
        {
            const std::string key = field.first.as<std::string>();
            if (!isKnownTilesetField(key))
            {
                issues.push_back({config_codec::Issue::Severity::warning, path + "." + key,
                                  "Unknown key; expected one of: name, path."});
            }
        }
    }

    return issues;
}

scope_settings_t load_settings(const std::string& path, std::string* problem)
{
    const auto report = [problem](std::string text)
    {
        if (problem != nullptr && problem->empty())
        {
            *problem = std::move(text);
        }
    };

    if (!QFileInfo::exists(QString::fromStdString(path)))
    {
        // A first run. Not logged as a problem: there is nothing wrong with
        // having no settings yet, and the file is written when something
        // changes rather than merely because the app started.
        SPDLOG_DEBUG("No settings at '{}'; using defaults.", path);
        return {};
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(path);

        bool fatal = false;
        for (const config_codec::Issue& issue : validate_settings(root))
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
            // Defaults, and the file is left alone. Scope still runs; the map
            // panel says its tileset is not configured, which points at the
            // real problem better than an app that refuses to start.
            SPDLOG_ERROR("Ignoring '{}': see the errors above. The file is NOT overwritten.",
                         path);
            report("The settings file was ignored (invalid content); defaults are in use and "
                   "the file was not overwritten.");
            return {};
        }

        scope_settings_t settings = root.as<scope_settings_t>();

        for (const std::string& note : checkTilesets(settings))
        {
            SPDLOG_WARN("{}: {}", path, note);
        }

        return settings;
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("Could not parse settings '{}': {}. Using defaults; the file is NOT "
                     "overwritten.", path, e.what());
        report(std::string("The settings file could not be parsed (") + e.what() +
               "); defaults are in use and the file was not overwritten.");
        return {};
    }
}

bool save_settings(const scope_settings_t& settings, const std::string& path)
{
    const QString qpath = QString::fromStdString(path);

    // The directory does not exist on a first save -- Qt reports the location
    // whether or not anything has created it.
    const QDir dir = QFileInfo(qpath).absoluteDir();
    if (!dir.exists() && !dir.mkpath("."))
    {
        SPDLOG_ERROR("Could not create settings directory '{}'.",
                     dir.absolutePath().toStdString());
        return false;
    }

    try
    {
        YAML::Emitter emitter;
        emitter << YAML::convert<scope_settings_t>::encode(settings);

        // QSaveFile writes to a temporary and renames on commit, so a crash or
        // a full disk leaves the previous file intact rather than a truncated
        // one. Truncated reads back as "no tilesets", which is indistinguishable
        // from a user who configured none.
        QSaveFile out(qpath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            SPDLOG_ERROR("Could not open '{}' for writing: {}", path,
                         out.errorString().toStdString());
            return false;
        }

        const std::string text = std::string(emitter.c_str()) + "\n";
        if (out.write(text.data(), static_cast<qint64>(text.size())) < 0 || !out.commit())
        {
            SPDLOG_ERROR("Failed while writing '{}': {}", path, out.errorString().toStdString());
            return false;
        }

        SPDLOG_INFO("Wrote settings to '{}'.", path);
        return true;
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("Could not encode settings for '{}': {}", path, e.what());
        return false;
    }
}

std::vector<std::string> checkTilesets(const scope_settings_t& settings)
{
    std::vector<std::string> notes;
    std::set<std::string> seen;

    for (const scope_tileset_t& tileset : settings.tilesets)
    {
        if (tileset.name.empty())
        {
            notes.push_back("A tileset has no name; nothing can refer to it.");
            continue;
        }

        // A name is what a panel config carries and what appears in a caption.
        // '/' would make it ambiguous with a path and with the zenoh key
        // grammar map_server uses for the same identifier.
        if (tileset.name.find('/') != std::string::npos)
        {
            notes.push_back("Tileset '" + tileset.name + "' contains '/', which is not allowed.");
        }

        if (!seen.insert(tileset.name).second)
        {
            // Two archives under one name: a panel asking for it would get
            // whichever came first, which is a silently wrong map rather than
            // a missing one.
            notes.push_back("Tileset '" + tileset.name +
                            "' is defined more than once; only the first is used.");
        }

        if (tileset.path.empty())
        {
            notes.push_back("Tileset '" + tileset.name + "' has no path.");
        }
    }

    return notes;
}

}  // namespace scope
