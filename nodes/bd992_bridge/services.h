// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's zenoh services, and the configuration pass they share with the
// node's own periodic check.
//
// run_config_pass() is the read-before-write cycle in one place: read what the
// receiver is doing, diff it against the YAML, and -- only in enforce mode,
// and only for what actually differs -- write the correction. main() calls it
// on connect and on a timer; the apply_config service calls the identical
// function, so a service call and a scheduled check can never disagree about
// what "configured correctly" means.

#ifndef BD992_NODE_SERVICES_H
#define BD992_NODE_SERVICES_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bd992.capnp.h"
#include "bd992/control_client.h"
#include "bd992/output_config.h"
#include "node_config.h"
#include "publishers.h"

namespace bd992_node
{

struct ConfigPass
{
    bool ok { false };
    std::string error;

    // What differed. Empty means the receiver was already configured as asked,
    // which is the case the whole design optimises for.
    std::vector<bd992::Change> changes;

    // False when nothing needed writing, when the mode is report-only, or when
    // the caller asked for a dry run.
    bool written { false };
};

// `dryRun` forces report-only for this pass. It cannot force the other way:
// a node configured for report_only will not write because a service asked it
// to.
ConfigPass run_config_pass(bd992::ControlClient& control, const NodeConfig& config, bool dryRun);

// Fill a schema builder from a change. Shared with the status message, so a
// drift reported by the service and the same drift on the status topic read
// identically.
void fillChange(::Bd992ConfigChange::Builder out, const bd992::Change& in);
void fillOutputMessage(::Bd992OutputMessage::Builder out, const bd992::OutputMessage& in);

// Owns the five queryables. Declared after everything they touch, undeclared
// before it, by lifetime.
class Services
{
  public:
    struct Deps
    {
        bd992::ControlClient* control { nullptr };
        Publishers* publishers { nullptr };
        const NodeConfig* config { nullptr };
        // Bumped by whichever path actually wrote to the receiver, so the
        // status message can report "something keeps changing this back".
        std::atomic<std::uint64_t>* outputsCorrected { nullptr };
    };

    Services(Deps deps);
    ~Services();

    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace bd992_node

#endif // BD992_NODE_SERVICES_H
