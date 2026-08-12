// SPDX-License-Identifier: GPL-3.0-or-later
//
// Desired versus actual: what the node decides to write, and — more often —
// what it decides not to.
//
// The case that matters most is `test_a_matching_configuration_writes_nothing`.
// A node that rewrote the configuration on every connect would restart the
// receiver's outputs every time anything reconnected, and "the node corrected
// a drift" would be a message nobody reads because it appears every minute.
// The whole read-before-write design exists so that line is rare and therefore
// worth something.

#include "bd992/output_config.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace bd992;
using gsof::RecordType;

constexpr PortIndex kPort = PortIndex::IpSocket1;
constexpr PortIndex kOtherPort = PortIndex::IpSocket2;

OutputMessage want(RecordType record, Frequency rate, PortIndex port = kPort)
{
    return gsof_output(port, record, rate);
}

std::size_t countOf(const std::vector<Change>& changes, ChangeKind kind)
{
    std::size_t n = 0;
    for (const Change& change : changes)
    {
        if (change.kind == kind)
        {
            ++n;
        }
    }
    return n;
}

// ============================================================================
// Nothing to do
// ============================================================================

void test_a_matching_configuration_writes_nothing()
{
    const std::vector<OutputMessage> desired {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::Velocity, Frequency::Hz10),
        want(RecordType::PositionType, Frequency::Hz1),
    };
    const std::vector<OutputMessage> actual = desired;

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(changes.empty(), "a receiver already configured correctly produces no changes");
    check(plan_writes(changes, PortPolicy::Additive).empty(),
          "and therefore nothing is written -- the point of reading first");
}

void test_order_does_not_matter()
{
    // The receiver reports its outputs in whatever order it stores them, which
    // is not the order the YAML lists them. A diff sensitive to that would
    // report every output as both missing and unexpected.
    const std::vector<OutputMessage> desired {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::Velocity, Frequency::Hz10),
        want(RecordType::AttitudeInfo, Frequency::Hz10),
    };
    const std::vector<OutputMessage> actual {
        want(RecordType::AttitudeInfo, Frequency::Hz10),
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::Velocity, Frequency::Hz10),
    };

    check(diff(actual, desired, kPort, PortPolicy::Additive).empty(),
          "the same outputs in a different order are not a difference");
}

// ============================================================================
// Drift
// ============================================================================

void test_a_rate_change_is_one_write_not_two()
{
    const std::vector<OutputMessage> desired { want(RecordType::LatLongHeight, Frequency::Hz10) };
    const std::vector<OutputMessage> actual { want(RecordType::LatLongHeight, Frequency::Hz1) };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(changes.size() == 1, "a rate change is one change");
    check(countOf(changes, ChangeKind::RateDrift) == 1, "and it is reported as drift, not as add plus remove");
    check(countOf(changes, ChangeKind::Unexpected) == 0,
          "the existing output must NOT also be reported as unexpected -- that would turn it off and on again");

    if (changes.size() == 1)
    {
        check(changes[0].actual.rate == Frequency::Hz1, "the change reports what the receiver had");
        check(changes[0].desired.rate == Frequency::Hz10, "and what it should have");
    }

    // One record: the protocol changes a rate in place.
    const std::vector<std::uint8_t> writes = plan_writes(changes, PortPolicy::Additive);
    check(writes.size() == gsof::appfile::kOutputMessageRecordSizeGsof,
          "a rate change is a single output message record");
    check(writes.size() >= 5 && writes[4] == static_cast<std::uint8_t>(Frequency::Hz10),
          "carrying the new rate");
}

void test_a_missing_output_is_added()
{
    const std::vector<OutputMessage> desired {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::AttitudeInfo, Frequency::Hz10),
    };
    const std::vector<OutputMessage> actual { want(RecordType::LatLongHeight, Frequency::Hz10) };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(changes.size() == 1 && changes[0].kind == ChangeKind::Missing, "the absent output is missing");
    check(changes[0].desired.gsofRecordType == static_cast<std::uint8_t>(RecordType::AttitudeInfo),
          "and names the record that is absent");

    const std::vector<std::uint8_t> writes = plan_writes(changes, PortPolicy::Additive);
    check(writes.size() == gsof::appfile::kOutputMessageRecordSizeGsof, "one record is written");
    check(writes[6] == static_cast<std::uint8_t>(RecordType::AttitudeInfo), "for the missing message only");
}

void test_an_output_the_receiver_has_turned_off_reads_as_missing()
{
    // A receiver reports a disabled output as an entry at rate Off rather than
    // dropping it. Calling that "drift" would be technically true and would
    // read wrongly: the message is not arriving at all.
    const std::vector<OutputMessage> desired { want(RecordType::Velocity, Frequency::Hz10) };
    const std::vector<OutputMessage> actual { want(RecordType::Velocity, Frequency::Off) };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(changes.size() == 1, "one change");
    check(changes[0].kind == ChangeKind::Missing, "an output at rate Off is missing, not drifted");
    check(countOf(changes, ChangeKind::Unexpected) == 0,
          "and an already-off output is never reported as unexpected");
}

// ============================================================================
// Outputs nobody asked for
// ============================================================================

void test_an_unexpected_output_is_reported_under_both_policies()
{
    const std::vector<OutputMessage> desired { want(RecordType::LatLongHeight, Frequency::Hz10) };
    const std::vector<OutputMessage> actual {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::AllSvDetailed, Frequency::Hz1),
    };

    // Reporting is the same either way, so `--check` shows the same picture
    // whatever mode the node is in.
    for (const PortPolicy policy : { PortPolicy::Additive, PortPolicy::Exclusive })
    {
        const std::vector<Change> changes = diff(actual, desired, kPort, policy);
        check(changes.size() == 1 && changes[0].kind == ChangeKind::Unexpected,
              std::string("the extra output is reported under the ") + to_string(policy) + " policy");
    }
}

void test_only_the_exclusive_policy_turns_an_unexpected_output_off()
{
    const std::vector<OutputMessage> desired { want(RecordType::LatLongHeight, Frequency::Hz10) };
    const std::vector<OutputMessage> actual {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::AllSvDetailed, Frequency::Hz1),
    };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(plan_writes(changes, PortPolicy::Additive).empty(),
          "additive leaves an output it did not ask for alone -- something else may be consuming it");

    const std::vector<std::uint8_t> exclusive = plan_writes(changes, PortPolicy::Exclusive);
    check(exclusive.size() == gsof::appfile::kOutputMessageRecordSizeGsof, "exclusive writes one record");
    check(exclusive.size() >= 7 && exclusive[4] == static_cast<std::uint8_t>(Frequency::Off),
          "turning it off");
    check(exclusive.size() >= 7 && exclusive[6] == static_cast<std::uint8_t>(RecordType::AllSvDetailed),
          "and naming the right record");
}

// ============================================================================
// Other ports are none of our business
// ============================================================================

void test_outputs_on_other_ports_are_ignored_entirely()
{
    // A receiver feeding corrections to an NTRIP server, or a second consumer
    // on another socket. Under an exclusive policy on OUR port, turning those
    // off would be an outage for something the node knows nothing about.
    const std::vector<OutputMessage> desired { want(RecordType::LatLongHeight, Frequency::Hz10) };
    const std::vector<OutputMessage> actual {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::AllSvDetailed, Frequency::Hz1, kOtherPort),
        want(RecordType::PositionTime, Frequency::Hz10, PortIndex::Serial1),
    };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Exclusive);

    check(changes.empty(), "outputs on other ports are not reported");
    check(plan_writes(changes, PortPolicy::Exclusive).empty(),
          "and an exclusive policy on one port never touches another");
}

void test_a_desired_output_on_another_port_is_not_our_problem_either()
{
    const std::vector<OutputMessage> desired {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::Velocity, Frequency::Hz10, kOtherPort),
    };
    const std::vector<OutputMessage> actual { want(RecordType::LatLongHeight, Frequency::Hz10) };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(changes.empty(),
          "diff considers one port, so an entry for another port is neither missing nor written");
}

// ============================================================================
// Mixed, and the shape of a real first connect
// ============================================================================

void test_a_factory_reset_receiver_gets_everything()
{
    // The scenario the whole feature exists for: someone reset the receiver,
    // and it is now silent.
    const std::vector<OutputMessage> desired {
        want(RecordType::PositionTime, Frequency::Hz10),
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::Velocity, Frequency::Hz10),
        want(RecordType::AttitudeInfo, Frequency::Hz10),
        want(RecordType::PositionType, Frequency::Hz1),
    };
    const std::vector<OutputMessage> actual {};

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(changes.size() == 5, "every configured output is missing");
    check(countOf(changes, ChangeKind::Missing) == 5, "all of them as Missing");

    const std::vector<std::uint8_t> writes = plan_writes(changes, PortPolicy::Additive);
    check(writes.size() == 5 * gsof::appfile::kOutputMessageRecordSizeGsof, "and five records are written");
    check(writes.size() <= gsof::appfile::kMaxAppFileRecordBytes,
          "a full configuration still fits in a single application file page");
}

void test_a_mixture_of_every_kind()
{
    const std::vector<OutputMessage> desired {
        want(RecordType::LatLongHeight, Frequency::Hz10),   // matches
        want(RecordType::Velocity, Frequency::Hz10),        // drifted to 1 Hz
        want(RecordType::AttitudeInfo, Frequency::Hz10),    // absent
    };
    const std::vector<OutputMessage> actual {
        want(RecordType::LatLongHeight, Frequency::Hz10),
        want(RecordType::Velocity, Frequency::Hz1),
        want(RecordType::AllSvDetailed, Frequency::Hz1),    // not asked for
    };

    const std::vector<Change> changes = diff(actual, desired, kPort, PortPolicy::Additive);

    check(countOf(changes, ChangeKind::Missing) == 1, "one missing");
    check(countOf(changes, ChangeKind::RateDrift) == 1, "one drifted");
    check(countOf(changes, ChangeKind::Unexpected) == 1, "one unexpected");
    check(changes.size() == 3, "and the matching output produced nothing");

    check(plan_writes(changes, PortPolicy::Additive).size() == 2 * gsof::appfile::kOutputMessageRecordSizeGsof,
          "additive writes the missing and the drifted, and leaves the unexpected alone");
    check(plan_writes(changes, PortPolicy::Exclusive).size() == 3 * gsof::appfile::kOutputMessageRecordSizeGsof,
          "exclusive also turns off the unexpected one");
}

void test_non_gsof_outputs_are_handled()
{
    // A port configured for NMEA as well. The four-byte record form has no
    // GSOF sub-record, and treating it as one would compare against a
    // meaningless byte.
    OutputMessage nmea {};
    nmea.outputType = OutputType::NmeaGga;
    nmea.port = kPort;
    nmea.rate = Frequency::Hz1;

    OutputMessage nmeaFaster = nmea;
    nmeaFaster.rate = Frequency::Hz5;

    const std::vector<OutputMessage> changes =
        std::vector<OutputMessage> { nmeaFaster };
    const std::vector<Change> diffed = diff(std::vector<OutputMessage> { nmea }, changes, kPort,
                                            PortPolicy::Additive);

    check(diffed.size() == 1 && diffed[0].kind == ChangeKind::RateDrift,
          "an NMEA output at the wrong rate is drift like any other");

    const std::vector<std::uint8_t> writes = plan_writes(diffed, PortPolicy::Additive);
    check(writes.size() == gsof::appfile::kOutputMessageRecordSizeSimple,
          "and is written as the SHORT record form, with no GSOF sub-record");
}

void test_change_descriptions_name_the_record()
{
    const std::vector<OutputMessage> desired { want(RecordType::AttitudeInfo, Frequency::Hz10) };
    const std::vector<Change> changes = diff({}, desired, kPort, PortPolicy::Additive);

    check(changes.size() == 1, "one change to describe");
    if (changes.empty())
    {
        return;
    }

    const std::string text = to_string(changes[0]);
    check(text.find("attitude_info") != std::string::npos,
          "a change reads as the record's name, not just its number: " + text);
    check(text.find("10hz") != std::string::npos, "and names the rate: " + text);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    test_a_matching_configuration_writes_nothing();
    test_order_does_not_matter();
    test_a_rate_change_is_one_write_not_two();
    test_a_missing_output_is_added();
    test_an_output_the_receiver_has_turned_off_reads_as_missing();
    test_an_unexpected_output_is_reported_under_both_policies();
    test_only_the_exclusive_policy_turns_an_unexpected_output_off();
    test_outputs_on_other_ports_are_ignored_entirely();
    test_a_desired_output_on_another_port_is_not_our_problem_either();
    test_a_factory_reset_receiver_gets_everything();
    test_a_mixture_of_every_kind();
    test_non_gsof_outputs_are_handled();
    test_change_descriptions_name_the_record();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all bd992 output configuration checks passed");
    return 0;
}
