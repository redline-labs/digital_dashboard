// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deciding what, if anything, to write to the device's output configuration.
//
// Free functions over two lists, with no serial port anywhere near them, for
// the reason bd992/output_config.h gives: this is the logic that decides
// whether the node touches a device's stored settings at all, and every case
// of it should be a plain unit test rather than something only a bench can
// reach.
//
// ONE THING IS DIFFERENT FROM THE BD992, and it shapes everything here.
// Trimble's APPFILE lets a single output be changed on its own. XBus does not:
// SetOutputConfiguration carries the WHOLE list and replaces the whole list.
// So "leave the outputs I did not mention alone" is not a matter of writing
// less -- it is a matter of writing MORE, merging what the device already has
// into the message. Getting that backwards silently switches off every output
// the node did not ask for, which on a shared device is somebody else's data
// disappearing with no error anywhere.

#ifndef MTI610_OUTPUT_CONFIG_H
#define MTI610_OUTPUT_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "xbus/commands.h"

namespace mti610
{

using xbus::OutputEntry;

enum class ConfigMode
{
    // Read the device's configuration, then write only what differs.
    Enforce,
    // Read and report the difference, change nothing. For a device somebody
    // else owns.
    ReportOnly,
};

enum class PortPolicy
{
    // Leave outputs that are not listed alone, and report them. The default:
    // the device may legitimately be feeding something else, and a list of
    // what THIS node needs is no basis for deciding those are wrong.
    Additive,
    // Turn off anything not listed. Only for a device this node owns outright.
    Exclusive,
};

enum class ChangeKind
{
    // In the desired list, absent from the device.
    Missing,
    // Present on both, at different rates.
    RateDrift,
    // On the device, absent from the desired list. Reported under Additive,
    // acted on under Exclusive.
    Unexpected,
};

struct Change
{
    ChangeKind kind { ChangeKind::Missing };

    // The identifier, format nibble included. Asking for 0x4020 and asking for
    // 0x4022 are different requests.
    std::uint16_t rawId { 0 };

    // For RateDrift, what the device has and what was wanted. Both are the
    // NORMALISED rates -- see normalise_frequency().
    std::uint16_t actualHz { 0 };
    std::uint16_t desiredHz { 0 };

    bool operator==(const Change&) const = default;
};

std::string to_string(ChangeKind kind);
std::string to_string(const Change& change);

// What the device will report for a requested frequency.
//
// Two things are folded in here, and both cause the same failure if they are
// not: a configuration check that compares un-normalised rates finds drift on
// every pass, rewrites the configuration, and does it again a minute later,
// forever. That loop only shows up against hardware, which is exactly why it
// is worth encoding now.
//
//   * 0x0000 and 0xFFFF both mean "as fast as you can", and the device answers
//     0xFFFF for both.
//   * For the timestamp and status identifiers the frequency is ignored
//     outright -- they accompany every packet -- and the device answers
//     0xFFFF whatever was asked.
std::uint16_t normalise_frequency(std::uint16_t rawId, std::uint16_t frequencyHz);

// Compare what the device reports against what the configuration asks for.
//
// `actual` is the list read back from ReqOutputConfiguration; `desired` is the
// list from YAML. The result is ordered: Missing and RateDrift first, in
// `desired` order, then Unexpected in `actual` order, so a log of it reads as
// "what is wrong with this device" rather than as a set difference.
std::vector<Change> diff(const std::vector<OutputEntry>& actual,
                         const std::vector<OutputEntry>& desired);

// The list to send, given what the device has and what is wanted.
//
// Returns the complete replacement list, because that is what
// SetOutputConfiguration carries. Under Additive the device's own entries that
// the desired list does not mention are carried through; under Exclusive they
// are dropped. Entries are emitted with their frequencies normalised, so a
// subsequent read-back compares equal without a second normalisation pass.
//
// An empty result under Exclusive is legal and means "output nothing" -- which
// is a real configuration, not an error, and the node refuses it at a higher
// level rather than here.
std::vector<OutputEntry> plan_writes(const std::vector<OutputEntry>& actual,
                                     const std::vector<OutputEntry>& desired,
                                     PortPolicy policy);

// True when the device already matches, so nothing needs writing. Under
// Additive an Unexpected entry does not count as a mismatch -- it is reported,
// not corrected.
bool is_satisfied(const std::vector<Change>& changes, PortPolicy policy);

} // namespace mti610

#endif // MTI610_OUTPUT_CONFIG_H
