// SPDX-License-Identifier: GPL-3.0-or-later

#include "bd992/output_config.h"

#include <algorithm>

namespace bd992
{

const char* to_string(PortPolicy policy)
{
    switch (policy)
    {
        case PortPolicy::Additive:  return "additive";
        case PortPolicy::Exclusive: return "exclusive";
    }

    return "unknown";
}

const char* to_string(ChangeKind kind)
{
    switch (kind)
    {
        case ChangeKind::Missing:    return "missing";
        case ChangeKind::RateDrift:  return "rate drift";
        case ChangeKind::Unexpected: return "unexpected";
    }

    return "unknown";
}

namespace
{

std::string describe(const OutputMessage& message)
{
    std::string out = gsof::appfile::to_string(message.outputType);

    if (message.isGsof)
    {
        out += " ";
        out += gsof::record_name(static_cast<gsof::RecordType>(message.gsofRecordType));
        out += "(";
        out += std::to_string(message.gsofRecordType);
        out += ")";
    }

    out += " on port ";
    out += std::to_string(static_cast<unsigned>(message.port));
    out += " at ";
    out += gsof::appfile::to_string(message.rate);

    return out;
}

} // namespace

std::string to_string(const Change& change)
{
    switch (change.kind)
    {
        case ChangeKind::Missing:
            return std::string("missing: ") + describe(change.desired);
        case ChangeKind::RateDrift:
            return std::string("rate drift: ") + describe(change.actual) + " -> " +
                   gsof::appfile::to_string(change.desired.rate);
        case ChangeKind::Unexpected:
            return std::string("unexpected: ") + describe(change.actual);
    }

    return "unknown change";
}

OutputMessage gsof_output(PortIndex port, gsof::RecordType record, Frequency rate, std::uint8_t offsetSeconds)
{
    OutputMessage out {};
    out.outputType = OutputType::Gsof;
    out.port = port;
    out.rate = rate;
    out.offsetSeconds = offsetSeconds;
    out.isGsof = true;
    out.gsofRecordType = static_cast<std::uint8_t>(record);
    out.gsofOffsetSeconds = 0;
    return out;
}

std::vector<Change> diff(std::span<const OutputMessage> actual, std::span<const OutputMessage> desired,
                         PortIndex port, PortPolicy policy)
{
    std::vector<Change> changes;

    const auto onPort = [port](const OutputMessage& message) { return message.port == port; };

    // Desired against actual: missing outputs, and outputs at the wrong rate.
    for (const OutputMessage& want : desired)
    {
        if (!onPort(want))
        {
            continue;
        }

        const auto found = std::find_if(actual.begin(), actual.end(), [&want, &onPort](const OutputMessage& have) {
            return onPort(have) && have.sameOutputAs(want);
        });

        if (found == actual.end())
        {
            changes.push_back(Change { ChangeKind::Missing, want, {} });
            continue;
        }

        // An output the receiver has but has turned off is missing, not
        // drifted: the receiver reports it with rate Off, and calling that a
        // rate change would be true but would read wrongly in a log.
        if (found->rate != want.rate)
        {
            changes.push_back(Change { found->rate == Frequency::Off ? ChangeKind::Missing : ChangeKind::RateDrift,
                                       want, *found });
        }
    }

    // Actual against desired: outputs nobody asked for.
    for (const OutputMessage& have : actual)
    {
        if (!onPort(have) || have.rate == Frequency::Off)
        {
            // An output that is already off is not something the receiver is
            // doing, so it is not unexpected -- it is just a leftover entry.
            continue;
        }

        const auto found = std::find_if(desired.begin(), desired.end(), [&have, &onPort](const OutputMessage& want) {
            return onPort(want) && want.sameOutputAs(have);
        });

        if (found == desired.end())
        {
            changes.push_back(Change { ChangeKind::Unexpected, {}, have });
        }
    }

    (void)policy;  // The policy decides what is DONE about a change, in
                   // plan_writes(); reporting is the same either way, so that
                   // `--check` shows the same picture whatever mode is set.

    return changes;
}

std::vector<std::uint8_t> plan_writes(std::span<const Change> changes, PortPolicy policy)
{
    std::vector<std::uint8_t> records;

    const auto append = [&records](std::span<const std::uint8_t> bytes) {
        records.insert(records.end(), bytes.begin(), bytes.end());
    };

    for (const Change& change : changes)
    {
        switch (change.kind)
        {
            case ChangeKind::Missing:
            case ChangeKind::RateDrift:
            {
                // Both are the same write. Turning an output on and changing
                // its rate are one record in this protocol, which is why the
                // node never has to remove-then-add and cannot leave a gap in
                // between.
                const OutputMessage& want = change.desired;
                if (want.isGsof)
                {
                    append(gsof::appfile::gsof_output_record(want.port, want.rate,
                                                             static_cast<gsof::RecordType>(want.gsofRecordType),
                                                             want.offsetSeconds, want.gsofOffsetSeconds));
                }
                else
                {
                    append(gsof::appfile::simple_output_record(want.outputType, want.port, want.rate,
                                                               want.offsetSeconds));
                }
                break;
            }

            case ChangeKind::Unexpected:
            {
                if (policy != PortPolicy::Exclusive)
                {
                    break;
                }

                const OutputMessage& have = change.actual;
                if (have.isGsof)
                {
                    append(gsof::appfile::gsof_output_off(have.port,
                                                          static_cast<gsof::RecordType>(have.gsofRecordType)));
                }
                else
                {
                    append(gsof::appfile::simple_output_record(have.outputType, have.port, Frequency::Off));
                }
                break;
            }
        }
    }

    return records;
}

} // namespace bd992
