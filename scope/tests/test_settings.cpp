// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-user settings: the codec, and the four failure modes that are silent.
//
// Every case here is one where the wrong behaviour looks like something else:
//
//   * a missing file reported as an error looks like a broken install, when it
//     is just a first run;
//   * a malformed file that gets OVERWRITTEN destroys the hand-edit that was
//     nearly right, and the user sees an empty list instead of their typo;
//   * a truncated write reads back as "no tilesets configured", which is
//     indistinguishable from a user who configured none;
//   * a duplicate name means a panel silently draws from whichever archive came
//     first -- a wrong map rather than a missing one.
//
// QCoreApplication is needed for QSaveFile and QStandardPaths, but no widgets
// and no bus, so this stays a plain unit test.

#include "scope/settings.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::filesystem::path tempDir()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "scope_test_settings";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path);
    out << text;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A first run has no settings. That must be defaults, not an error -- an app
// that reports a problem on a clean machine trains people to ignore it.
void testAMissingFileIsDefaultsNotAnError()
{
    const auto dir = tempDir();
    const scope::scope_settings_t settings =
        scope::load_settings((dir / "absent.yaml").string());

    expect(settings.tilesets.empty(), "a missing settings file yields no tilesets");
    expect(!std::filesystem::exists(dir / "absent.yaml"),
           "loading a missing settings file does not create it");
}

void testEveryFieldSurvivesARoundTrip()
{
    const auto dir = tempDir();
    const auto path = (dir / "scope.yaml").string();

    scope::scope_settings_t written;
    scope::scope_tileset_t socal;
    socal.name = "socal";
    socal.path = "/data/socal.mbtiles";
    scope::scope_tileset_t tracks;
    tracks.name = "tracks";
    tracks.path = "/data/tracks.mbtiles";
    written.tilesets = {socal, tracks};

    expect(scope::save_settings(written, path), "save_settings reports success");

    const scope::scope_settings_t read = scope::load_settings(path);
    expect(read.tilesets.size() == 2, "both tilesets survive the round trip");
    expect(read.tilesets[0].name == "socal" && read.tilesets[0].path == "/data/socal.mbtiles",
           "the first tileset's name and path survive");
    expect(read.tilesets[1].name == "tracks" && read.tilesets[1].path == "/data/tracks.mbtiles",
           "the second tileset's name and path survive");
}

// Saving twice must produce the same bytes. A codec that round-trips
// semantically but reorders keys turns every settings change into a diff of the
// whole file, which is exactly what the workspace codec is pinned against too.
void testSavingTwiceIsByteStable()
{
    const auto dir = tempDir();
    const auto first = (dir / "a.yaml").string();
    const auto second = (dir / "b.yaml").string();

    scope::scope_settings_t settings;
    scope::scope_tileset_t socal;
    socal.name = "socal";
    socal.path = "/data/socal.mbtiles";
    settings.tilesets.push_back(socal);

    scope::save_settings(settings, first);
    scope::save_settings(scope::load_settings(first), second);

    expect(readFile(first) == readFile(second), "saving a loaded file reproduces it byte for byte");
}

// THE ONE THAT MATTERS MOST. A file with a typo in it is worth more than the
// empty file that would replace it, so a parse failure must leave it alone.
void testAMalformedFileIsNotOverwritten()
{
    const auto dir = tempDir();
    const auto path = dir / "scope.yaml";
    const std::string original = "tilesets: [ this is not: valid: yaml\n";
    writeFile(path, original);

    const scope::scope_settings_t settings = scope::load_settings(path.string());
    expect(settings.tilesets.empty(), "an unparseable settings file yields defaults");
    expect(readFile(path) == original, "an unparseable settings file is left on disk untouched");
}

void testATopLevelSequenceIsRejected()
{
    const auto dir = tempDir();
    const auto path = dir / "scope.yaml";
    writeFile(path, "- socal\n- tracks\n");

    const scope::scope_settings_t settings = scope::load_settings(path.string());
    expect(settings.tilesets.empty(), "a top-level sequence yields defaults");
    expect(readFile(path) == "- socal\n- tracks\n", "a rejected settings file is not overwritten");
}

void testTilesetsMustBeASequence()
{
    const YAML::Node root = YAML::Load("tilesets:\n  name: socal\n");
    const auto issues = scope::validate_settings(root);

    bool sawError = false;
    for (const auto& issue : issues)
    {
        sawError = sawError || issue.severity == config_codec::Issue::Severity::error;
    }
    expect(sawError, "a mapping under 'tilesets' is an error");
}

// A misspelt key is invisible to the decoder -- it is driven by the struct's
// fields -- so the setting simply never takes effect. Saying so is the whole
// job of validate_settings.
void testAnUnknownKeyIsAWarningNotAnError()
{
    const YAML::Node root = YAML::Load("tilesets:\n  - name: socal\n    paht: /data/x.mbtiles\n");
    const auto issues = scope::validate_settings(root);

    bool sawWarning = false;
    bool sawError = false;
    for (const auto& issue : issues)
    {
        sawWarning = sawWarning || issue.severity == config_codec::Issue::Severity::warning;
        sawError = sawError || issue.severity == config_codec::Issue::Severity::error;
    }
    expect(sawWarning, "a misspelt tileset key is reported");
    expect(!sawError, "a misspelt tileset key does not stop the file loading");
}

void testCheckTilesetsFindsTheThreeWaysANameGoesWrong()
{
    scope::scope_settings_t settings;
    scope::scope_tileset_t duplicate_a;
    duplicate_a.name = "socal";
    duplicate_a.path = "/data/a.mbtiles";
    scope::scope_tileset_t duplicate_b;
    duplicate_b.name = "socal";
    duplicate_b.path = "/data/b.mbtiles";
    scope::scope_tileset_t slashed;
    slashed.name = "maps/socal";
    slashed.path = "/data/c.mbtiles";
    scope::scope_tileset_t unnamed;
    unnamed.path = "/data/d.mbtiles";
    scope::scope_tileset_t pathless;
    pathless.name = "empty";
    settings.tilesets = {duplicate_a, duplicate_b, slashed, unnamed, pathless};

    const auto notes = scope::checkTilesets(settings);

    const auto mentions = [&notes](const std::string& needle) {
        for (const std::string& note : notes)
        {
            if (note.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };

    expect(mentions("more than once"), "a duplicate tileset name is reported");
    expect(mentions("'/'"), "a tileset name containing a slash is reported");
    expect(mentions("no name"), "an unnamed tileset is reported");
    expect(mentions("no path"), "a tileset with no path is reported");
}

// A clean list must produce NO notes. A checker that always says something is a
// checker nobody reads.
void testCheckTilesetsIsSilentOnACleanList()
{
    scope::scope_settings_t settings;
    scope::scope_tileset_t socal;
    socal.name = "socal";
    socal.path = "/data/socal.mbtiles";
    settings.tilesets.push_back(socal);

    expect(scope::checkTilesets(settings).empty(), "a well-formed tileset list produces no notes");
}

// The default path has to land under the organization and application names, or
// what a user saved will not be found on the next run.
void testTheDefaultPathFollowsTheApplicationName()
{
    const std::string path = scope::settingsPath();
    expect(path.find("redline") != std::string::npos,
           "the default settings path contains the organization name");
    expect(path.find("scope") != std::string::npos,
           "the default settings path contains the application name");
    expect(path.rfind("scope.yaml") == path.size() - std::string("scope.yaml").size(),
           "the default settings path ends in scope.yaml");
}

// The directory does not exist on a first save: Qt reports the location whether
// or not anything created it.
void testSavingCreatesTheDirectory()
{
    const auto dir = tempDir();
    const auto path = (dir / "nested" / "deeper" / "scope.yaml").string();

    scope::scope_settings_t settings;
    scope::scope_tileset_t socal;
    socal.name = "socal";
    socal.path = "/data/socal.mbtiles";
    settings.tilesets.push_back(socal);

    expect(scope::save_settings(settings, path), "saving into a missing directory succeeds");
    expect(std::filesystem::exists(path), "the settings file is there afterwards");
    expect(scope::load_settings(path).tilesets.size() == 1, "and reads back");
}

}  // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::off);

    // QStandardPaths derives the settings location from these, and QSaveFile
    // needs an application object to exist at all.
    QCoreApplication::setOrganizationName("redline");
    QCoreApplication::setApplicationName("scope");
    QCoreApplication app(argc, argv);

    testAMissingFileIsDefaultsNotAnError();
    testEveryFieldSurvivesARoundTrip();
    testSavingTwiceIsByteStable();
    testAMalformedFileIsNotOverwritten();
    testATopLevelSequenceIsRejected();
    testTilesetsMustBeASequence();
    testAnUnknownKeyIsAWarningNotAnError();
    testCheckTilesetsFindsTheThreeWaysANameGoesWrong();
    testCheckTilesetsIsSilentOnACleanList();
    testTheDefaultPathFollowsTheApplicationName();
    testSavingCreatesTheDirectory();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
