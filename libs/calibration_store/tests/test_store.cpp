// SPDX-License-Identifier: GPL-3.0-or-later
//
// The store doing its job: rows go in and come back exactly, the newest row
// matching a group AND its prior hash AND its model version is the one
// found, nothing is ever replaced, and all of it survives a reopen.

#include "common.h"

using test::check;
namespace cs = calibration_store;

namespace
{

void testRoundTrip()
{
    test::TempDir dir("roundtrip");
    const auto file = dir.path / "nested" / "deeper" / "calibration.sqlite";
    auto store = cs::Store::open(file);
    check(store.has_value(), "opens, making the directories it needs");
    if (!store) return;
    const auto in = test::row("lever_arm", "f36fa677ab23fffc", 0.3);
    const auto id = store->append(in);
    check(id.has_value() && *id > 0, "appends");

    const auto out = store->latest("lever_arm", "f36fa677ab23fffc", 1);
    check(out.has_value() && out->has_value(), "and finds it");
    if (!out || !*out) return;
    const auto& r = **out;
    check(r.mean == in.mean && r.cov == in.cov, "numbers come back exactly");
    check(r.session == in.session && r.summary == in.summary && r.reason == in.reason &&
              r.written_at_ns == in.written_at_ns && r.evidence_s == in.evidence_s && r.drive_s == in.drive_s,
          "and so does everything else");
}

void testLatestMatchesAllThree()
{
    test::TempDir dir("latest");
    auto store = cs::Store::open(dir.path / "c.sqlite");
    if (!store) return check(false, "opens");
    store->append(test::row("lever_arm", "aaaa", 0.30));
    store->append(test::row("lever_arm", "aaaa", 0.31));
    store->append(test::row("lever_arm", "bbbb", 0.50));      // re-measured since
    store->append(test::row("lever_arm", "aaaa", 0.99, 2));   // a newer model
    store->append(test::row("mounting", "aaaa", 0.77));       // another group, same hash
    // Two in the same second both kept, and ordered by id, not by time.
    auto same = test::row("lever_arm", "aaaa", 0.32);
    same.written_at_ns -= 1'000'000'000;  // a wall clock that stepped back
    store->append(same);

    const auto a = store->latest("lever_arm", "aaaa", 1);
    check(a && *a && (**a).mean[0] == 0.32, "newest by id for the hash and version, even with an older timestamp");
    const auto b = store->latest("lever_arm", "bbbb", 1);
    check(b && *b && (**b).mean[0] == 0.50, "another hash has its own newest");
    const auto v2 = store->latest("lever_arm", "aaaa", 2);
    check(v2 && *v2 && (**v2).mean[0] == 0.99, "and another model version");
    const auto none = store->latest("lever_arm", "cccc", 1);
    check(none && !*none, "a hash nothing was learned against finds nothing, which is not an error");

    const auto h = store->history("lever_arm");
    check(h && h->size() == 5, "history keeps every row of the group, whatever its hash");
    bool ordered = true;
    for (std::size_t i = 1; h && i < h->size(); ++i) ordered = ordered && (*h)[i].id > (*h)[i - 1].id;
    check(ordered, "oldest first");
}

void testReopen()
{
    test::TempDir dir("reopen");
    const auto file = dir.path / "c.sqlite";
    {
        auto store = cs::Store::open(file);
        if (!store) return check(false, "opens");
        store->append(test::row("boresight", "abcd", 0.01));
    }
    auto again = cs::Store::open(file);
    check(again.has_value(), "reopens");
    if (!again) return;
    const auto r = again->latest("boresight", "abcd", 1);
    check(r && *r && (**r).mean[0] == 0.01, "and what was written is still there");
    again->append(test::row("boresight", "abcd", 0.02));
    const auto h = again->history("boresight");
    check(h && h->size() == 2, "appending after a reopen adds, never replaces");
}

void testCallerCheck()
{
    test::TempDir dir("accept");
    auto store = cs::Store::open(dir.path / "c.sqlite");
    if (!store) return check(false, "opens");
    store->append(test::row("mounting", "h", 0.1));
    store->append(test::row("mounting", "h", 0.2));
    std::vector<std::string> skipped;
    const auto r = store->latest(
        "mounting", "h", 1,
        [](const cs::Row& row) -> std::optional<std::string> {
            if (row.mean[0] > 0.15) return "the caller does not like it";
            return std::nullopt;
        },
        &skipped);
    check(r && *r && (**r).mean[0] == 0.1, "a row the caller refuses is passed over for the one before");
    check(skipped.size() == 1 && skipped[0].find("does not like") != std::string::npos, "and says why");
}

}  // namespace

int main()
{
    testRoundTrip();
    testLatestMatchesAllThree();
    testReopen();
    testCallerCheck();
    if (test::failures)
    {
        SPDLOG_ERROR("{} failure(s)", test::failures);
        return 1;
    }
    SPDLOG_INFO("calibration store: all passed");
    return 0;
}
