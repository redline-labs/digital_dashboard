// SPDX-License-Identifier: GPL-3.0-or-later

#include "mti610/output_config.h"

#include <algorithm>

namespace mti610
{
namespace
{

const OutputEntry* findById(const std::vector<OutputEntry>& entries, std::uint16_t rawId)
{
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [rawId](const OutputEntry& e) { return e.rawId == rawId; });
    return it == entries.end() ? nullptr : &*it;
}

std::string hex16(std::uint16_t value)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 12; shift >= 0; shift -= 4)
    {
        out += kDigits[(value >> shift) & 0xF];
    }
    return out;
}

std::string rate(std::uint16_t hz)
{
    return hz == xbus::kMaxFrequency ? std::string("max") : std::to_string(hz) + " Hz";
}

} // namespace

std::string to_string(ChangeKind kind)
{
    switch (kind)
    {
        case ChangeKind::Missing:    return "missing";
        case ChangeKind::RateDrift:  return "rate drift";
        case ChangeKind::Unexpected: return "unexpected";
    }

    return "unknown";
}

std::string to_string(const Change& change)
{
    const std::string id = hex16(change.rawId) + " (" +
                           xbus::data_name(static_cast<xbus::DataId>(
                               xbus::data_type(change.rawId))) +
                           ")";

    switch (change.kind)
    {
        case ChangeKind::Missing:
            return id + " is not enabled; wanted at " + rate(change.desiredHz);
        case ChangeKind::RateDrift:
            return id + " is at " + rate(change.actualHz) + "; wanted " + rate(change.desiredHz);
        case ChangeKind::Unexpected:
            return id + " is enabled at " + rate(change.actualHz) + " and was not asked for";
    }

    return id;
}

std::uint16_t normalise_frequency(std::uint16_t rawId, std::uint16_t frequencyHz)
{
    if (xbus::frequency_is_ignored(rawId))
    {
        return xbus::kMaxFrequency;
    }

    // "Selecting an Output Frequency of either 0x0000 or 0xFFFF makes the
    // device select the maximum frequency", and it answers 0xFFFF for both.
    if (frequencyHz == 0)
    {
        return xbus::kMaxFrequency;
    }

    return frequencyHz;
}

std::vector<Change> diff(const std::vector<OutputEntry>& actual,
                         const std::vector<OutputEntry>& desired)
{
    std::vector<Change> changes;

    for (const OutputEntry& want : desired)
    {
        const std::uint16_t wantHz = normalise_frequency(want.rawId, want.frequencyHz);
        const OutputEntry* have = findById(actual, want.rawId);

        if (have == nullptr)
        {
            changes.push_back(Change { ChangeKind::Missing, want.rawId, 0, wantHz });
            continue;
        }

        const std::uint16_t haveHz = normalise_frequency(have->rawId, have->frequencyHz);
        if (haveHz != wantHz)
        {
            changes.push_back(Change { ChangeKind::RateDrift, want.rawId, haveHz, wantHz });
        }
    }

    for (const OutputEntry& have : actual)
    {
        if (findById(desired, have.rawId) != nullptr)
        {
            continue;
        }

        changes.push_back(Change { ChangeKind::Unexpected, have.rawId,
                                   normalise_frequency(have.rawId, have.frequencyHz), 0 });
    }

    return changes;
}

std::vector<OutputEntry> plan_writes(const std::vector<OutputEntry>& actual,
                                     const std::vector<OutputEntry>& desired,
                                     PortPolicy policy)
{
    std::vector<OutputEntry> planned;
    planned.reserve(desired.size() + actual.size());

    for (const OutputEntry& want : desired)
    {
        planned.push_back(OutputEntry { want.rawId,
                                        normalise_frequency(want.rawId, want.frequencyHz) });
    }

    if (policy == PortPolicy::Additive)
    {
        // Carry through what the device already had. THIS IS THE WHOLE
        // DIFFERENCE between the two policies: SetOutputConfiguration replaces
        // the entire list, so leaving an output alone means re-sending it.
        for (const OutputEntry& have : actual)
        {
            if (findById(desired, have.rawId) != nullptr)
            {
                continue;
            }

            planned.push_back(OutputEntry { have.rawId,
                                            normalise_frequency(have.rawId, have.frequencyHz) });
        }
    }

    return planned;
}

bool is_satisfied(const std::vector<Change>& changes, PortPolicy policy)
{
    for (const Change& change : changes)
    {
        switch (change.kind)
        {
            case ChangeKind::Missing:
            case ChangeKind::RateDrift:
                return false;

            case ChangeKind::Unexpected:
                // Reported but not corrected under Additive. Treating it as a
                // mismatch there would make the node rewrite a shared device's
                // configuration on every check.
                if (policy == PortPolicy::Exclusive)
                {
                    return false;
                }
                break;
        }
    }

    return true;
}

} // namespace mti610
