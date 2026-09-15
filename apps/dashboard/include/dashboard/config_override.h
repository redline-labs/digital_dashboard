#ifndef DASHBOARD_CONFIG_OVERRIDE_H_
#define DASHBOARD_CONFIG_OVERRIDE_H_

#include "dashboard/app_config.h"

#include <optional>
#include <string>

// Two configs, two failure classes.
//
// The SHIPPED config lives in the read-only rootfs (the RAUC slot). If it does
// not load, the software is broken: the dashboard exits, never reports READY,
// and the slot rolls back. That is the right outcome and nothing here changes it.
//
// An OVERRIDE is a config an operator put on the writable data partition to
// change the cluster without reflashing. Both slots share that partition, so a
// rollback cannot cure a bad override; instead the dashboard rejects it, runs
// the shipped config, still reports READY (the slot is fine), and says so:
// journal, systemd STATUS, and a banner on the screen the operator is looking
// at. The override file itself is never touched, so the edit is not lost.
//
// Rejection covers what the loader refuses AND a window whose widgets fail to
// build (main.cpp decides that after construction). A third case validation
// cannot see is an override that crashes the process after loading: that would
// restart-loop, never reach READY, and roll BOTH slots back in turn. So a start
// with an override writes an attempt marker next to it and removes it once the
// first frame is on screen; a marker already present at startup means the last
// attempt never got there, and the override is skipped this boot.
namespace dashboard::config
{

struct Selection
{
    dashboard_config_t config;
    // The file the config came from.
    std::string path;
    bool override_in_use = false;
    // Why the override was not used, when one was given and it was not.
    std::optional<std::string> override_rejected;
};

// <override path>.attempt
std::string attemptMarkerPath(const std::string& override_path);

// Loads the override if it is given, exists, has no attempt marker and loads;
// otherwise the shipped config, with the reason recorded. nullopt only when the
// shipped config itself fails to load, which the caller treats as fatal.
std::optional<Selection> select(const std::string& shipped_path,
                                const std::optional<std::string>& override_path);

// After the first frame is on screen with an override: the attempt succeeded.
void clearAttemptMarker(const std::string& override_path);

// The override loaded but could not be built (widgets failed): record it and
// fall back. Returns the shipped selection, or nullopt if even that fails.
std::optional<Selection> rejectAfterLoad(const Selection& failed, const std::string& shipped_path,
                                         const std::string& reason);

// One line for the journal, systemd STATUS and the on-screen banner.
std::string describe(const Selection& selection);

}  // namespace dashboard::config

#endif  // DASHBOARD_CONFIG_OVERRIDE_H_
