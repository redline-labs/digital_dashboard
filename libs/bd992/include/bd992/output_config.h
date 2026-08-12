// SPDX-License-Identifier: GPL-3.0-or-later
//
// Comparing what the receiver is outputting against what the config asks for.
//
// This is the whole of the read-before-write requirement, and it is
// deliberately a pair of free functions over two lists with no I/O in them.
// Every interesting case -- nothing drifted, a rate changed, a record went
// missing, the receiver has outputs nobody asked for -- is then a plain unit
// test, and the same code answers both modes: report-only stops after diff(),
// enforce carries on to plan_writes().
//
// WHY THE NODE DOES NOT SIMPLY WRITE THE DESIRED CONFIGURATION EVERY TIME:
// applying an application file is not free. It restarts outputs, and on a
// receiver shared with anything else it is a visible event. Writing only what
// actually drifted means a healthy system is never disturbed, and it makes
// "the node corrected something" a signal worth logging rather than noise
// emitted at every connect.

#ifndef BD992_OUTPUT_CONFIG_H
#define BD992_OUTPUT_CONFIG_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gsof/commands.h"
#include "gsof/records.h"

namespace bd992
{

using gsof::appfile::Frequency;
using gsof::appfile::OutputMessage;
using gsof::appfile::OutputType;
using gsof::appfile::PortIndex;

// What to do about outputs the receiver has that the configuration does not
// mention.
enum class PortPolicy
{
    // Leave them alone, and report them. The default, because a receiver may
    // legitimately be feeding something else -- an NTRIP server, a second
    // consumer on another socket -- and a node whose config lists what IT
    // needs has no basis for deciding those are wrong.
    Additive,
    // Turn them off. For a port this node owns outright.
    Exclusive,
};

const char* to_string(PortPolicy policy);

enum class ChangeKind
{
    // The configuration asks for an output the receiver does not have.
    Missing,
    // The receiver has the output, at a different rate.
    RateDrift,
    // The receiver has an output the configuration does not mention. Only
    // ever actionable under the exclusive policy.
    Unexpected,
};

const char* to_string(ChangeKind kind);

struct Change
{
    ChangeKind kind { ChangeKind::Missing };

    // Meaningful for Missing and RateDrift.
    OutputMessage desired {};
    // Meaningful for RateDrift and Unexpected.
    OutputMessage actual {};
};

// One line describing a change, for a log or a service response.
std::string to_string(const Change& change);

// Build the output message a configuration entry asks for.
OutputMessage gsof_output(PortIndex port, gsof::RecordType record, Frequency rate,
                          std::uint8_t offsetSeconds = 0);

// Compare, considering only outputs on `port`.
//
// Outputs on other ports are ignored entirely, in both directions: the node
// configures one port and must not report -- let alone disable -- a
// correction stream feeding something else on another.
std::vector<Change> diff(std::span<const OutputMessage> actual, std::span<const OutputMessage> desired,
                         PortIndex port, PortPolicy policy);

// The application file records that correct `changes`, ready to hand to
// encode_application_file().
//
// Returns empty when there is nothing to do, which is the case the node checks
// before sending anything at all. Unexpected changes are only acted on when
// the policy says so, so passing the full diff from an additive run correctly
// produces no removals.
std::vector<std::uint8_t> plan_writes(std::span<const Change> changes, PortPolicy policy);

} // namespace bd992

#endif // BD992_OUTPUT_CONFIG_H
