// SPDX-License-Identifier: GPL-3.0-or-later
//
// The logic that decides whether the node writes to a device's stored settings.
//
// Pure functions over two lists, so every case here is reachable without a
// port. Two of them are worth more than the rest:
//
//   * testAdditiveCarriesThroughWhatItDidNotAskFor(). XBus has no way to
//     change one output: SetOutputConfiguration replaces the whole list. So
//     "leave the outputs I did not mention alone" means re-sending them, and
//     getting it backwards switches off somebody else's data with no error
//     anywhere.
//
//   * testFrequencyNormalisationStopsTheRewriteLoop(). The device forces
//     0xFFFF on the timestamp and status identifiers whatever was asked for.
//     A check that compared the raw numbers would find drift on every pass,
//     rewrite, and find it again a minute later -- forever. That loop is
//     invisible without hardware, which is exactly why it is pinned here.

#include "mti610/output_config.h"

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

using namespace mti610;

// Identifiers used throughout, with the format nibble that the node's YAML
// would produce.
constexpr std::uint16_t kAccel = 0x4022;      // fp16.32
constexpr std::uint16_t kGyro = 0x8022;       // fp16.32
constexpr std::uint16_t kMag = 0xC022;        // fp16.32
constexpr std::uint16_t kTemperature = 0x0810;
constexpr std::uint16_t kPacketCounter = 0x1020;
constexpr std::uint16_t kStatusWord = 0xE020;

bool hasId(const std::vector<OutputEntry>& entries, std::uint16_t rawId)
{
    for (const OutputEntry& entry : entries)
    {
        if (entry.rawId == rawId) { return true; }
    }
    return false;
}

std::uint16_t rateOf(const std::vector<OutputEntry>& entries, std::uint16_t rawId)
{
    for (const OutputEntry& entry : entries)
    {
        if (entry.rawId == rawId) { return entry.frequencyHz; }
    }
    return 0;
}

std::size_t countOf(const std::vector<Change>& changes, ChangeKind kind)
{
    std::size_t n = 0;
    for (const Change& change : changes)
    {
        if (change.kind == kind) { ++n; }
    }
    return n;
}

void testAnAlreadyCorrectDeviceIsLeftAlone()
{
    const std::vector<OutputEntry> configured { { kAccel, 100 }, { kGyro, 100 } };

    const std::vector<Change> changes = diff(configured, configured);
    check(changes.empty(), "a device that already matches has nothing to change");
    check(is_satisfied(changes, PortPolicy::Additive), "and is satisfied under additive");
    check(is_satisfied(changes, PortPolicy::Exclusive), "and under exclusive");
}

void testMissingAndDrift()
{
    const std::vector<OutputEntry> actual { { kAccel, 50 } };
    const std::vector<OutputEntry> desired { { kAccel, 100 }, { kGyro, 100 } };

    const std::vector<Change> changes = diff(actual, desired);
    check(changes.size() == 2, "one drift and one missing");
    check(countOf(changes, ChangeKind::RateDrift) == 1, "acceleration is at the wrong rate");
    check(countOf(changes, ChangeKind::Missing) == 1, "rate of turn is not enabled at all");

    check(changes[0].kind == ChangeKind::RateDrift && changes[0].actualHz == 50 &&
              changes[0].desiredHz == 100,
          "and the drift reports both rates, so a log says what to fix");

    check(!is_satisfied(changes, PortPolicy::Additive), "which is not satisfied");
}

void testUnexpectedIsReportedButNotCorrectedUnderAdditive()
{
    // The device is feeding something else as well. Under additive that is
    // reported and left alone -- a list of what THIS node needs is no basis
    // for deciding somebody else's output is wrong.
    const std::vector<OutputEntry> actual { { kAccel, 100 }, { kMag, 50 } };
    const std::vector<OutputEntry> desired { { kAccel, 100 } };

    const std::vector<Change> changes = diff(actual, desired);
    check(countOf(changes, ChangeKind::Unexpected) == 1, "the extra output is reported");
    check(is_satisfied(changes, PortPolicy::Additive), "but does not count as a mismatch");
    check(!is_satisfied(changes, PortPolicy::Exclusive), "unless the node owns the device");
}

void testAdditiveCarriesThroughWhatItDidNotAskFor()
{
    // THE TEST THIS FILE EXISTS FOR. SetOutputConfiguration replaces the whole
    // list, so an additive plan has to re-send the entries it is leaving
    // alone. A plan that contained only the desired list would silently switch
    // the magnetic field off.
    const std::vector<OutputEntry> actual { { kAccel, 50 }, { kMag, 50 } };
    const std::vector<OutputEntry> desired { { kAccel, 100 }, { kGyro, 100 } };

    const std::vector<OutputEntry> planned = plan_writes(actual, desired, PortPolicy::Additive);

    check(planned.size() == 3, "the plan holds both wanted entries and the one to preserve");
    check(hasId(planned, kAccel) && rateOf(planned, kAccel) == 100,
          "acceleration at the rate that was asked for");
    check(hasId(planned, kGyro) && rateOf(planned, kGyro) == 100, "rate of turn added");
    check(hasId(planned, kMag) && rateOf(planned, kMag) == 50,
          "and the magnetic field carried through at the rate the device had");

    // The same inputs under exclusive.
    const std::vector<OutputEntry> exclusive = plan_writes(actual, desired, PortPolicy::Exclusive);
    check(exclusive.size() == 2, "an exclusive plan holds only what was asked for");
    check(!hasId(exclusive, kMag), "and drops the magnetic field");
}

void testFrequencyNormalisationStopsTheRewriteLoop()
{
    // The device ignores the frequency on the timestamp and status
    // identifiers, answering 0xFFFF whatever it was asked. A YAML that says
    // 100 Hz for the packet counter therefore reads back as 0xFFFF, and a
    // comparison of the raw numbers would call that drift on every pass.
    const std::vector<OutputEntry> actual {
        { kPacketCounter, xbus::kMaxFrequency },
        { kStatusWord, xbus::kMaxFrequency },
        { kAccel, 100 },
    };

    const std::vector<OutputEntry> desired {
        { kPacketCounter, 100 },   // what a careless YAML says
        { kStatusWord, 0 },        // and what a careful one says
        { kAccel, 100 },
    };

    const std::vector<Change> changes = diff(actual, desired);
    check(changes.empty(), "a frequency the device ignores is not drift");
    check(is_satisfied(changes, PortPolicy::Exclusive), "so nothing is rewritten");

    // And the plan writes the normalised value, so the read-back after it
    // compares equal without a second normalisation pass.
    const std::vector<OutputEntry> planned = plan_writes(actual, desired, PortPolicy::Exclusive);
    check(rateOf(planned, kPacketCounter) == xbus::kMaxFrequency,
          "the plan sends what the device will report");
    check(rateOf(planned, kStatusWord) == xbus::kMaxFrequency, "for both of them");
    check(rateOf(planned, kAccel) == 100, "and leaves a real rate alone");
}

void testZeroMeansMaximum()
{
    // "Selecting an Output Frequency of either 0x0000 or 0xFFFF makes the
    // device select the maximum frequency." Both must normalise the same way,
    // or a YAML that says 0 rewrites the configuration forever.
    check(normalise_frequency(kAccel, 0) == xbus::kMaxFrequency, "zero means maximum");
    check(normalise_frequency(kAccel, xbus::kMaxFrequency) == xbus::kMaxFrequency,
          "and so does 0xFFFF");
    check(normalise_frequency(kAccel, 100) == 100, "a real rate is left alone");

    const std::vector<OutputEntry> actual { { kAccel, xbus::kMaxFrequency } };
    const std::vector<OutputEntry> desired { { kAccel, 0 } };
    check(diff(actual, desired).empty(), "so the two spellings agree");
}

void testTheFormatNibbleIsPartOfTheIdentity()
{
    // Asking for 0x4020 (float32) and asking for 0x4022 (fp16.32) are
    // different requests. Treating them as the same output would leave the
    // device sending float32 while the parser was told to expect fp16.32 --
    // which decodes without error, and is wrong.
    const std::vector<OutputEntry> actual { { 0x4020, 100 } };
    const std::vector<OutputEntry> desired { { 0x4022, 100 } };

    const std::vector<Change> changes = diff(actual, desired);
    check(countOf(changes, ChangeKind::Missing) == 1, "a different format is a missing output");
    check(countOf(changes, ChangeKind::Unexpected) == 1, "and the old one is unexpected");
    check(!is_satisfied(changes, PortPolicy::Exclusive), "so it gets corrected");
}

void testEmptyCases()
{
    const std::vector<OutputEntry> none;
    const std::vector<OutputEntry> some { { kAccel, 100 } };

    // A device fresh from the factory with nothing enabled.
    const std::vector<Change> fromEmpty = diff(none, some);
    check(fromEmpty.size() == 1 && fromEmpty[0].kind == ChangeKind::Missing,
          "everything is missing on a device outputting nothing");

    // A node configured to ask for nothing. Legal -- the node refuses it one
    // level up, where there is a config file to point at.
    const std::vector<Change> toEmpty = diff(some, none);
    check(toEmpty.size() == 1 && toEmpty[0].kind == ChangeKind::Unexpected,
          "and everything is unexpected when nothing was asked for");
    check(plan_writes(some, none, PortPolicy::Exclusive).empty(),
          "an exclusive plan for nothing is empty, which is a real configuration");
    check(plan_writes(some, none, PortPolicy::Additive).size() == 1,
          "while an additive one preserves what was there");
}

void testChangeDescriptionsNameTheOutput()
{
    // A log line has to be actionable without a hex table to hand.
    const Change missing { ChangeKind::Missing, kAccel, 0, 100 };
    const std::string text = to_string(missing);

    check(text.find("acceleration") != std::string::npos, "a change names the output");
    check(text.find("0x4022") != std::string::npos, "and the identifier it asked for");
    check(text.find("100 Hz") != std::string::npos, "and the rate");

    const Change maxRate { ChangeKind::RateDrift, kStatusWord, 100, xbus::kMaxFrequency };
    check(to_string(maxRate).find("max") != std::string::npos,
          "0xFFFF reads as 'max' rather than as 65535 Hz");

    check(to_string(Change { ChangeKind::Unexpected, kTemperature, 1, 0 }).find("not asked for") !=
              std::string::npos,
          "and an unexpected output says so");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testAnAlreadyCorrectDeviceIsLeftAlone();
    testMissingAndDrift();
    testUnexpectedIsReportedButNotCorrectedUnderAdditive();
    testAdditiveCarriesThroughWhatItDidNotAskFor();
    testFrequencyNormalisationStopsTheRewriteLoop();
    testZeroMeansMaximum();
    testTheFormatNibbleIsPartOfTheIdentity();
    testEmptyCases();
    testChangeDescriptionsNameTheOutput();

    return failures == 0 ? 0 : 1;
}
