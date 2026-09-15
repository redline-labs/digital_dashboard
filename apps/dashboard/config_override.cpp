#include "dashboard/config_override.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace dashboard::config
{

std::string attemptMarkerPath(const std::string& override_path)
{
    return override_path + ".attempt";
}

namespace
{

std::optional<Selection> loadShipped(const std::string& shipped_path, std::optional<std::string> rejected)
{
    auto cfg = load_dashboard_config(shipped_path);
    if (!cfg)
    {
        return std::nullopt;
    }
    Selection selection;
    selection.config = std::move(*cfg);
    selection.path = shipped_path;
    selection.override_rejected = std::move(rejected);
    return selection;
}

}  // namespace

std::optional<Selection> select(const std::string& shipped_path, const std::optional<std::string>& override_path)
{
    std::error_code ec;
    if (!override_path || !std::filesystem::exists(*override_path, ec))
    {
        return loadShipped(shipped_path, std::nullopt);
    }

    const std::string marker = attemptMarkerPath(*override_path);
    if (std::filesystem::exists(marker, ec))
    {
        return loadShipped(shipped_path,
                           "the previous start with this override never reached the screen (" + marker +
                               " is still there); remove the marker to try it again");
    }

    auto cfg = load_dashboard_config(*override_path);
    if (!cfg)
    {
        return loadShipped(shipped_path, "it does not load (the errors above say why)");
    }

    // Written only once the file has loaded, so a typo does not leave a marker
    // behind; cleared by clearAttemptMarker() after the first frame.
    std::ofstream(marker) << "started\n";

    Selection selection;
    selection.config = std::move(*cfg);
    selection.path = *override_path;
    selection.override_in_use = true;
    return selection;
}

void clearAttemptMarker(const std::string& override_path)
{
    std::error_code ec;
    std::filesystem::remove(attemptMarkerPath(override_path), ec);
}

std::optional<Selection> rejectAfterLoad(const Selection& failed, const std::string& shipped_path,
                                         const std::string& reason)
{
    // The attempt is over, and it was a rejection rather than a crash, so the
    // marker must not stay and block the next try after the operator fixes it.
    clearAttemptMarker(failed.path);
    return loadShipped(shipped_path, reason);
}

std::string describe(const Selection& selection)
{
    if (selection.override_rejected)
    {
        return "config override rejected: " + *selection.override_rejected + "; running the shipped config " +
               selection.path;
    }
    if (selection.override_in_use)
    {
        return "config override in use: " + selection.path;
    }
    return "config: " + selection.path;
}

}  // namespace dashboard::config
