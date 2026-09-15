// Which config the dashboard runs, and the attempt marker. Exercises the real
// loader against files in a temp directory; no display.
#include "dashboard/config_override.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition)
    {
        ++failures;
    }
}

const char* kGood = R"(
name: shipped
width: 600
height: 300
widgets: []
)";

const char* kOverride = R"(
name: operator
width: 800
height: 400
widgets: []
)";

const char* kBad = "name: broken\nwidth: -5\nwidgets: [\n";

void write(const std::filesystem::path& p, const char* text)
{
    std::ofstream(p) << text;
}

}  // namespace

int main()
{
    namespace fs = std::filesystem;
    using namespace dashboard::config;

    const fs::path dir = fs::temp_directory_path() / "dashboard_test_config_override";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string shipped = (dir / "shipped.yaml").string();
    const std::string override_path = (dir / "override.yaml").string();
    write(shipped, kGood);

    {
        const auto s = select(shipped, std::nullopt);
        check(s && !s->override_in_use && !s->override_rejected && s->path == shipped,
              "no override given: the shipped config, nothing rejected");
    }
    {
        const auto s = select(shipped, override_path);
        check(s && !s->override_in_use && !s->override_rejected && s->path == shipped,
              "an override path that does not exist is not a rejection");
        check(!fs::exists(attemptMarkerPath(override_path)), "and no marker is written");
    }
    {
        write(override_path, kBad);
        const auto s = select(shipped, override_path);
        check(s && !s->override_in_use && s->override_rejected && s->path == shipped,
              "a bad override is rejected and the shipped config runs");
        check(!fs::exists(attemptMarkerPath(override_path)), "a bad override leaves no marker");
        check(describe(*s).find("rejected") != std::string::npos, "describe() says rejected: " + describe(*s));
        check(fs::exists(override_path), "the rejected file is left in place");
    }
    {
        write(override_path, kOverride);
        const auto s = select(shipped, override_path);
        check(s && s->override_in_use && s->path == override_path && s->config.windows.size() == 1 &&
                  s->config.windows[0].name == "operator",
              "a good override is used");
        check(fs::exists(attemptMarkerPath(override_path)), "and the attempt marker is written");
        clearAttemptMarker(override_path);
        check(!fs::exists(attemptMarkerPath(override_path)), "clearAttemptMarker removes it");
    }
    {
        write(attemptMarkerPath(override_path), "started\n");
        const auto s = select(shipped, override_path);
        check(s && !s->override_in_use && s->override_rejected && s->override_rejected->find("never reached") != std::string::npos,
              "a leftover marker means the last attempt crashed: override skipped");
        check(fs::exists(attemptMarkerPath(override_path)), "and the marker stays until someone removes it");
        fs::remove(attemptMarkerPath(override_path));
    }
    {
        auto s = select(shipped, override_path);
        check(s && s->override_in_use, "override in use again once the marker is gone");
        const auto fallback = rejectAfterLoad(*s, shipped, "2 widget(s) failed to build");
        check(fallback && !fallback->override_in_use && fallback->override_rejected &&
                  fallback->override_rejected->find("widget") != std::string::npos,
              "a post-load rejection falls back to the shipped config with the reason");
        check(!fs::exists(attemptMarkerPath(override_path)), "and clears the marker (it was not a crash)");
    }
    {
        // A good override does not need the shipped config at all...
        const auto s = select((dir / "missing.yaml").string(), override_path);
        check(s && s->override_in_use, "a good override is used even when the shipped config is missing");
        clearAttemptMarker(override_path);
        // ...but a bad one falls back, and then the shipped config is fatal.
        write(override_path, kBad);
        const auto fatal = select((dir / "missing.yaml").string(), override_path);
        check(!fatal, "a bad override plus a shipped config that does not load is fatal");
    }

    fs::remove_all(dir);
    std::printf("%s\n", failures == 0 ? "all config override tests passed" : "config override tests FAILED");
    return failures == 0 ? 0 : 1;
}
