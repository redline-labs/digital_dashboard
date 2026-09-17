// SPDX-License-Identifier: GPL-3.0-or-later
//
// TriggerEdge: when a sample changes the page. The case that matters most is
// the first one -- a keypad reports its whole state, so a button held at
// startup, or a node that restarts, must not look like a press.

#include "page_stack/trigger_edge.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using page_stack::TriggerEdge;
using namespace std::chrono_literals;

const TriggerEdge::clock::time_point t0{};

void testRisingIgnoresTheFirstSample()
{
    TriggerEdge edge(trigger_edge_t::rising, 0ms);
    expect(!edge.primed(), "not primed before any sample");
    expect(!edge.onSample(1.0, t0), "a first sample that is already true does not fire");
    expect(edge.primed(), "but it primes");
    expect(!edge.onSample(1.0, t0 + 10ms), "held true does not fire");
    expect(!edge.onSample(0.0, t0 + 20ms), "falling does not fire");
    expect(edge.onSample(1.0, t0 + 30ms), "0 -> 1 fires");
    expect(!edge.onSample(1.0, t0 + 40ms), "and fires once");
}

void testRisingFromAFalseFirstSample()
{
    TriggerEdge edge(trigger_edge_t::rising, 0ms);
    expect(!edge.onSample(0.0, t0), "a false first sample does not fire");
    expect(edge.onSample(2.0, t0 + 10ms), "any non-zero value counts as true");
}

void testRisingNeverReprimesWithoutATimeout()
{
    // A change-only source: silence for a minute, then the press arrives.
    TriggerEdge edge(trigger_edge_t::rising, 0ms);
    edge.onSample(0.0, t0);
    expect(edge.onSample(1.0, t0 + 60s), "the first press after a long silence still fires");
}

void testRisingReprimesAfterAGapWhenAsked()
{
    TriggerEdge edge(trigger_edge_t::rising, 500ms);
    edge.onSample(0.0, t0);
    expect(edge.onSample(1.0, t0 + 400ms), "within the gap, a rise fires");
    edge.onSample(0.0, t0 + 800ms);
    // The publisher went away and came back already true.
    expect(!edge.onSample(1.0, t0 + 2s), "after a gap longer than the timeout, the next sample only primes");
    expect(!edge.onSample(1.0, t0 + 2100ms), "held true still does not fire");
    edge.onSample(0.0, t0 + 2200ms);
    expect(edge.onSample(1.0, t0 + 2300ms), "a real rise after re-priming fires");
}

void testOnSampleFiresOnEveryTruthySampleIncludingTheFirst()
{
    TriggerEdge edge(trigger_edge_t::on_sample, 0ms);
    expect(edge.onSample(1.0, t0), "an event topic fires on its first message");
    expect(edge.onSample(1.0, t0 + 1ms), "and on the next identical one");
    expect(!edge.onSample(0.0, t0 + 2ms), "a false result does not fire");
}

void testNonFiniteIsNotASample()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    TriggerEdge edge(trigger_edge_t::rising, 0ms);
    expect(!edge.onSample(nan, t0), "NaN does not fire");
    expect(!edge.primed(), "and does not prime");
    edge.onSample(0.0, t0 + 1ms);
    expect(!edge.onSample(nan, t0 + 2ms), "NaN between samples does not fire");
    expect(edge.onSample(1.0, t0 + 3ms), "and does not disturb the edge");

    TriggerEdge event(trigger_edge_t::on_sample, 0ms);
    expect(!event.onSample(std::numeric_limits<double>::infinity(), t0), "infinity is not an event");
}

}  // namespace

int main()
{
    testRisingIgnoresTheFirstSample();
    testRisingFromAFalseFirstSample();
    testRisingNeverReprimesWithoutATimeout();
    testRisingReprimesAfterAGapWhenAsked();
    testOnSampleFiresOnEveryTruthySampleIncludingTheFirst();
    testNonFiniteIsNotASample();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
