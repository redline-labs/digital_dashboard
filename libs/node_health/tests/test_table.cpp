// SPDX-License-Identifier: GPL-3.0-or-later
//
// HealthTable: the monitor's bookkeeping with the clock supplied, so every
// time is made up rather than waited for. The browser runs this same code, so
// a rule that is wrong here is wrong in both places at once.
#include "node_health/codec.h"
#include "node_health/health_json.h"
#include "node_health/table.h"

#include "check.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <string>

using namespace std::chrono_literals;
using node_health::Clock;
using node_health::HealthRow;
using node_health::HealthSnapshot;
using node_health::HealthTable;
using node_health::State;
using node_health::Verdict;

namespace
{

const Clock::time_point t0 = Clock::time_point(1h);
constexpr std::string_view kSchema = "NodeHealth";

HealthSnapshot report(std::string node, std::string zid, State state, std::uint64_t sequence,
                      std::uint64_t uptime_ms = 10'000)
{
    HealthSnapshot snapshot;
    snapshot.node = std::move(node);
    snapshot.zid = std::move(zid);
    snapshot.state = state;
    snapshot.sequence = sequence;
    snapshot.uptime_ms = uptime_ms;
    snapshot.period_ms = 1000;
    snapshot.pid = 42;
    return snapshot;
}

std::vector<std::uint8_t> encoded(const HealthSnapshot& snapshot)
{
    capnp::MallocMessageBuilder message;
    node_health::encode(message.initRoot<::NodeHealth>(), snapshot);
    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const auto bytes = words.asBytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::string verdictOf(const HealthTable& table, Clock::time_point now, std::size_t index = 0)
{
    const std::vector<HealthRow> rows = table.rows(now);
    if (index >= rows.size())
    {
        return "<no row>";
    }
    return std::string(node_health::to_string(rows[index].verdict));
}

void testIdentityJoinsSample()
{
    HealthTable table;
    table.identity("z1", "can_bridge", true, t0);
    test::check(table.sample(kSchema, encoded(report("can-bridge-old-name", "z1", State::ok, 1)), "",
                             t0 + 100ms),
                "a NodeHealth sample is accepted");

    const std::vector<HealthRow> rows = table.rows(t0 + 600ms);
    test::check(rows.size() == 1, "identity and sample for one zid are one row");
    if (rows.empty())
    {
        return;
    }
    test::check(rows[0].name == "can_bridge", "the identity's name wins over the sample's");
    test::check(rows[0].verdict == Verdict::ok, "a fresh ok sample is ok");
    test::check(rows[0].age == 500ms, "age is measured from when the sample arrived");
}

void testSampleBeforeIdentity()
{
    HealthTable table;
    test::check(table.sample(kSchema, encoded(report("map_server", "z2", State::degraded, 1)), "", t0),
                "accepted with no identity yet");
    const std::vector<HealthRow> rows = table.rows(t0);
    test::check(rows.size() == 1 && rows[0].name == "map_server",
                "the name comes from the sample until the directory catches up");
    test::check(verdictOf(table, t0) == "degraded", "and its own state is reported");
}

void testRejectsWhatIsNotNodeHealth()
{
    HealthTable table;
    const std::vector<std::uint8_t> good = encoded(report("x", "z3", State::ok, 1));
    test::check(!table.sample("CanFrame", good, "", t0),
                "a different schema under a health key is refused, even if it would decode");
    test::check(!table.sample("", good, "", t0), "no schema at all is refused");
    test::check(!table.sample(kSchema, {}, "z3", t0), "an empty payload is refused");
    test::check(!table.sample(kSchema, {0xff, 0x00, 0x13, 0x37, 0x01}, "z3", t0),
                "garbage is refused");
    test::check(table.rows(t0).empty(), "nothing refused makes a row");
    test::check(table.revision() == 0, "and nothing refused moves the revision");
}

void testZidFallsBackToOrigin()
{
    HealthTable table;
    test::check(table.sample(kSchema, encoded(report("old_node", "", State::ok, 1)), "origin9", t0),
                "a sample without its own zid takes the sender's");
    const std::vector<HealthRow> rows = table.rows(t0);
    test::check(rows.size() == 1 && rows[0].zid == "origin9", "keyed by the origin zid");
    test::check(!table.sample(kSchema, encoded(report("anon", "", State::ok, 1)), "", t0),
                "no zid anywhere is refused rather than keyed on empty");
}

void testSilentIsDatedFromFirstSighting()
{
    HealthTable table;
    table.identity("z4", "backlight", true, t0);
    test::check(verdictOf(table, t0 + 2s) == "starting", "within the grace it is starting");
    // Re-announcing the identity -- the monitor does this on every poll -- must
    // not restart the grace, or a hung node would read as starting forever.
    table.identity("z4", "backlight", true, t0 + 5s);
    test::check(verdictOf(table, t0 + 5s) == "silent", "past the grace it is silent");
}

void testExitedAndGone()
{
    HealthTable clean;
    clean.identity("z5", "carplay", true, t0);
    clean.sample(kSchema, encoded(report("carplay", "z5", State::stopping, 7)), "", t0 + 1s);
    clean.identity("z5", "carplay", false, t0 + 2s);
    test::check(verdictOf(clean, t0 + 2s) == "exited", "stopping then gone is a clean exit");

    HealthTable died;
    died.identity("z6", "carplay", true, t0);
    died.sample(kSchema, encoded(report("carplay", "z6", State::ok, 7)), "", t0 + 1s);
    died.identity("z6", "carplay", false, t0 + 2s);
    test::check(verdictOf(died, t0 + 2s) == "gone", "ok then gone is a death");
}

void testLate()
{
    HealthTable table;
    table.identity("z7", "megasquirt", true, t0);
    table.sample(kSchema, encoded(report("megasquirt", "z7", State::ok, 1)), "", t0);
    test::check(verdictOf(table, t0 + 2900ms) == "ok", "inside three periods it is ok");
    test::check(verdictOf(table, t0 + 3100ms) == "late", "past three periods it is late");
}

void testRestartsAreCounted()
{
    HealthTable table;
    table.sample(kSchema, encoded(report("n", "z8", State::ok, 10, 50'000)), "", t0);
    table.sample(kSchema, encoded(report("n", "z8", State::ok, 11, 51'000)), "", t0 + 1s);
    table.sample(kSchema, encoded(report("n", "z8", State::starting, 1, 200)), "", t0 + 2s);
    const std::vector<HealthRow> rows = table.rows(t0 + 2s);
    test::check(rows.size() == 1 && rows[0].continuity.restarts == 1,
                "a sequence that went backwards is one restart");
}

void testTwoInstancesAreTwoRows()
{
    HealthTable table;
    table.identity("zb", "dashboard", true, t0);
    table.identity("za", "dashboard", true, t0);
    table.identity("zc", "alpha", true, t0);
    const std::vector<HealthRow> rows = table.rows(t0);
    test::check(rows.size() == 3, "keyed by session, not name");
    test::check(rows.size() == 3 && rows[0].name == "alpha" && rows[1].zid == "za" &&
                    rows[2].zid == "zb",
                "sorted by name, then session");
}

void testRevision()
{
    HealthTable table;
    table.identity("z9", "n", true, t0);
    const std::uint64_t first = table.revision();
    table.identity("z9", "n", true, t0 + 1s);
    test::check(table.revision() == first, "re-announcing an unchanged identity does not move it");
    table.identity("z9", "n", false, t0 + 2s);
    test::check(table.revision() > first, "an identity going away does");
    const std::uint64_t second = table.revision();
    table.sample(kSchema, encoded(report("n", "z9", State::ok, 1)), "", t0);
    test::check(table.revision() > second, "an accepted sample does");
}

void testReportJson()
{
    HealthTable table;
    table.identity("z1", "quiet", true, t0);
    HealthSnapshot snapshot = report("loud", "z2", State::degraded, 3);
    snapshot.checks = {{"can_rx", State::degraded, "nothing for 2.0 s", 2000}};
    table.sample(kSchema, encoded(snapshot), "", t0);

    const nlohmann::json out = node_health::healthReportJson(table.rows(t0 + 1s), table.revision());
    test::check(out.value("bus_available", false), "a report says the bus is available");
    test::check(out["revision"] == table.revision(), "and carries the revision");
    test::check(out["nodes"].is_array() && out["nodes"].size() == 2, "one entry per row");
    if (!out["nodes"].is_array() || out["nodes"].size() != 2)
    {
        return;
    }

    const nlohmann::json& loud = out["nodes"][0];
    test::check(loud["name"] == "loud" && loud["verdict"] == "degraded" && loud["healthy"] == false,
                "verdict and healthy come from the classifier");
    test::check(loud["state"] == "degraded" && loud["sequence"] == 3 && loud["age_ms"] == 1000,
                "the sample's fields and its age");
    test::check(loud["checks"].size() == 1 && loud["checks"][0]["state"] == "degraded" &&
                    loud["checks"][0]["detail"] == "nothing for 2.0 s",
                "checks travel with names, states and details");

    const nlohmann::json& quiet = out["nodes"][1];
    test::check(quiet["verdict"] == "starting" && quiet["healthy"] == true,
                "an identity within its grace is starting, and healthy");
    test::check(quiet["state"].is_null() && quiet["age_ms"].is_null() && quiet["checks"].empty(),
                "never reported: null state and age, no checks -- not zeros");
}

}  // namespace

int main()
{
    testIdentityJoinsSample();
    testSampleBeforeIdentity();
    testRejectsWhatIsNotNodeHealth();
    testZidFallsBackToOrigin();
    testSilentIsDatedFromFirstSighting();
    testExitedAndGone();
    testLate();
    testRestartsAreCounted();
    testTwoInstancesAreTwoRows();
    testRevision();
    testReportJson();
    return test::finish();
}
