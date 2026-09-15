#ifndef SCOPE_SETTINGS_H_
#define SCOPE_SETTINGS_H_

// Per-user settings: what is true about THIS MACHINE, as opposed to what is
// true about a workspace.
//
// THE DIVIDING LINE, and it is the whole reason this file exists separately
// from workspace.h: a workspace says what to show, and is meant to be shared,
// committed and opened on someone else's laptop. Settings say where things
// are on disk here. A map panel therefore carries `tileset: socal` and NEVER
// a path -- the path lives here, and the name is what joins the two. Put a
// path in a workspace and it stops opening anywhere but the machine that
// wrote it.
//
// Written to the platform's per-user config location (see settingsPath), not
// to configs/. Nothing in this tree used QStandardPaths before this; the
// resolved paths are printed in docs/scope.md so there is one place to look
// when a setting appears not to stick.
#include "config_codec/config_validation.h"
#include "config_codec/config_yaml.h"
#include "reflection/reflection.h"

#include <yaml-cpp/yaml.h>

#include <string>
#include <vector>

namespace scope
{

// One map archive on this machine.
REFLECT_STRUCT(scope_tileset_t,
    (std::string, name, "",
        "Name", "What a map panel's 'tileset' refers to. Unique; may not contain '/'"),
    (std::string, path, "",
        "Path", "Absolute path to the .mbtiles archive")
)

REFLECT_STRUCT(scope_settings_t,
    (std::vector<scope_tileset_t>, tilesets, {},
        "Tilesets", "Map archives on this machine, by the name panels refer to them by"),
    (std::vector<std::string>, recent_workspaces, {},
        "Recent Workspaces", "Most recent first. Per-user because a path on this machine is"),
    (std::vector<std::string>, recent_recordings, {},
        "Recent Recordings", "Most recent first, same rule"),
    (std::string, last_directory, "",
        "Last Directory", "Where the file dialogs open. Convenience, safe to delete"),
    (std::string, window_geometry, "",
        "Window Geometry", "QWidget::saveGeometry() as base64. Opaque; safe to delete")
)

// Where settings live when --settings was not given:
//
//   Linux   ~/.config/redline/scope/scope.yaml
//   macOS   ~/Library/Preferences/redline/scope/scope.yaml
//
// Derived from QStandardPaths::AppConfigLocation, so it follows whatever the
// platform says rather than being hardcoded. Requires the QApplication's
// organization and application names to have been set; main() does that before
// anything reads settings.
std::string settingsPath();

// Absent file means defaults, and that is NOT an error -- a first run has no
// settings and must not be made to look like a failure. A file that exists but
// cannot be parsed is a warning plus defaults, and is deliberately NOT
// overwritten until the user changes something: a hand-edit with a typo in it
// is worth more than the empty file that would replace it.
// `problem`, when given, receives a one-line human-readable reason when an
// EXISTING file could not be used (malformed, or refused by validation) -- the
// case a user should hear about, because the Settings dialog then shows an
// empty table that reads as "never configured". A missing file sets nothing.
scope_settings_t load_settings(const std::string& path, std::string* problem = nullptr);

// Writes via a temporary and renames, so an interrupted write cannot leave a
// truncated file behind. A half-written settings file reads as "no tilesets
// configured", which looks exactly like a user who never configured any.
bool save_settings(const scope_settings_t& settings, const std::string& path);

std::vector<config_codec::Issue> validate_settings(const YAML::Node& root);

// Duplicate names, names containing '/', and empty names, as human-readable
// notes. Returned rather than thrown because the settings dialog shows them
// beside the row that caused them, and startup logs them and carries on: one
// bad row must not cost the user the archives that are fine.
//
// nodes/map_server refuses the same three at startup. It can afford to -- it
// has nothing else to do -- and here the equivalent would be an app that will
// not open.
std::vector<std::string> checkTilesets(const scope_settings_t& settings);

}  // namespace scope

#endif  // SCOPE_SETTINGS_H_
