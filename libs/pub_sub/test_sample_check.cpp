// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a typed consumer does with a sample, from what its publisher stamped.
//
// The verdict is constexpr and pinned by static_asserts: if this builds, the
// rule is right. The gate on top adds only the once-only logging, so the runtime
// half checks that its answers are the verdict's.

#include "pub_sub/sample_check.h"

#include "pub_sub/schema_layout.h"

#include <cstdio>
#include <string>

namespace
{

using pub_sub::judgeSample;
using pub_sub::SampleVerdict;

constexpr std::uint64_t kLayout = 0x1234;

static_assert(judgeSample("EngineRpm", kLayout, "EngineRpm", kLayout) == SampleVerdict::accept);
static_assert(judgeSample("EngineRpm", kLayout, "EngineRpm", std::nullopt) == SampleVerdict::accept,
              "an unstamped revision predates fingerprints and is decoded");
static_assert(judgeSample("EngineRpm", kLayout, "", kLayout) == SampleVerdict::unnamed,
              "a publisher that named no schema is decoded as expected");
static_assert(judgeSample("EngineRpm", kLayout, "VehicleSpeed", kLayout) ==
              SampleVerdict::wrong_schema);
static_assert(judgeSample("EngineRpm", kLayout, "zenoh/bytes", std::nullopt) ==
                  SampleVerdict::wrong_schema,
              "an encoding that is not ours is not an unnamed one");
static_assert(judgeSample("EngineRpm", kLayout, "EngineRpm", kLayout + 1) ==
              SampleVerdict::wrong_layout);
static_assert(judgeSample("EngineRpm", pub_sub::kNoLayout, "EngineRpm", kLayout) ==
                  SampleVerdict::accept,
              "a schema this build does not know is not a mismatch");
static_assert(judgeSample("EngineRpm", kLayout, "VehicleSpeed", kLayout + 1) ==
                  SampleVerdict::wrong_schema,
              "the name is judged before the revision");

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main()
{
    pub_sub::SampleGate gate("a/key", "EngineRpm", kLayout);
    expect(gate.admit("EngineRpm", kLayout), "admits the expected schema");
    expect(gate.admit("", std::nullopt), "admits an unnamed sample");
    expect(!gate.admit("VehicleSpeed", kLayout), "refuses another schema");
    expect(!gate.admit("VehicleSpeed", kLayout), "and keeps refusing it after saying so once");
    expect(!gate.admit("EngineRpm", kLayout + 1), "refuses another revision");
    expect(gate.admitRevision(kLayout), "admitRevision passes this build's revision");
    expect(!gate.admitRevision(kLayout + 1), "and refuses another");
    gate.refusePartialWord(7);
    gate.refuseMalformed("test");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
