// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the store does with things that are not what they should be. A bad
// row costs that row: the next older good one is used. A file that is not
// ours is refused and left as it was, to the byte -- the one thing worse than
// losing the calibration is destroying whatever else the path pointed at.

#include "common.h"

#include <sqlite3.h>

#include <fstream>
#include <iterator>
#include <sys/stat.h>

using test::check;
namespace cs = calibration_store;

namespace
{

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Writes a row behind the store's back, as a hand edit or a torn write would.
void rawInsert(const std::filesystem::path& file, const std::string& mean, const std::string& cov)
{
    sqlite3* db = nullptr;
    sqlite3_open(file.string().c_str(), &db);
    const std::string sql = "INSERT INTO calibration(written_at_ns, session, grp, prior_hash, model_version, mean, "
                            "cov, summary, reason, evidence_s, drive_s) VALUES(0,'x','lever_arm','h',1,'" +
                            mean + "','" + cov + "','','',0,0)";
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
    {
        SPDLOG_ERROR("raw insert failed: {}", err ? err : "?");
        sqlite3_free(err);
    }
    sqlite3_close(db);
}

void testBadRowsAreSkipped()
{
    test::TempDir dir("badrows");
    const auto file = dir.path / "c.sqlite";
    {
        auto store = cs::Store::open(file);
        if (!store) return check(false, "opens");
        store->append(test::row("lever_arm", "h", 0.30));  // the good one, oldest
    }
    const std::string I = "[1e-4,0,0,0,1e-4,0,0,0,1e-4]";
    rawInsert(file, "[0.3, 0, 1.2", I);                              // torn JSON
    rawInsert(file, "{\"x\": 0.3}", I);                              // not an array
    rawInsert(file, "[0.3, null, 1.2]", I);                          // NaN as JSON writes it
    rawInsert(file, "[]", I);                                        // empty
    rawInsert(file, "[0.3, 0, 1.2]", "[1e-4,0,0,1e-4]");             // 2x2 for a 3-vector is the caller's call...
    rawInsert(file, "[0.3, 0, 1.2]", "[1e-4,0,0,0,1e-4,0,0,0]");     // ...but 8 values is not square
    rawInsert(file, "[0.3, 0, 1.2]", "[1e-4,1,0,0,1e-4,0,0,0,1e-4]");   // asymmetric
    rawInsert(file, "[0.3, 0, 1.2]", "[1e-4,0,0,0,-1e-4,0,0,0,1e-4]");  // not positive definite
    rawInsert(file, "[1e999, 0, 1.2]", I);                           // overflows to inf

    auto store = cs::Store::open(file);
    if (!store) return check(false, "reopens");
    std::vector<std::string> skipped;
    const auto three = [](const cs::Row& r) -> std::optional<std::string> {
        if (r.mean.size() != 3 || r.cov.size() != 9) return "not a 3-vector with a 3x3 covariance";
        return std::nullopt;
    };
    const auto r = store->latest("lever_arm", "h", 1, three, &skipped);
    check(r.has_value(), "a store full of bad rows still answers");
    check(r && *r && (**r).mean[0] == 0.30, "with the newest GOOD row");
    check(skipped.size() == 9, "having passed over all nine bad ones: " + std::to_string(skipped.size()));
    for (const auto& s : skipped) SPDLOG_INFO("skipped: {}", s);
    const auto h = store->history("lever_arm");
    check(h && h->size() == 10, "the history still shows every row that is there");
}

void testNotADatabase()
{
    test::TempDir dir("notdb");
    const auto file = dir.path / "calibration.sqlite";
    {
        std::ofstream f(file, std::ios::binary);
        f << "This is somebody's notes, not a database.\n" << std::string(8000, 'x');
    }
    const std::string before = readAll(file);
    auto store = cs::Store::open(file);
    check(!store.has_value() && store.error().kind == cs::Error::Kind::NotADatabase, "a text file is refused");
    check(readAll(file) == before, "and left byte for byte as it was");
    check(!std::filesystem::exists(file.string() + "-wal"), "with nothing written beside it");
}

void testNewerSchema()
{
    test::TempDir dir("newer");
    const auto file = dir.path / "c.sqlite";
    {
        auto store = cs::Store::open(file);
        if (!store) return check(false, "opens");
        store->append(test::row("lever_arm", "h", 0.3));
    }
    sqlite3* db = nullptr;
    sqlite3_open(file.string().c_str(), &db);
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE); UPDATE meta SET value='2' WHERE name='schema_version'; PRAGMA wal_checkpoint(TRUNCATE);",
                 nullptr, nullptr, nullptr);
    sqlite3_close(db);
    const std::string before = readAll(file);
    auto store = cs::Store::open(file);
    check(!store.has_value() && store.error().kind == cs::Error::Kind::NewerSchema,
          "a database from a newer schema is refused");
    check(readAll(file) == before, "and not upgraded, downgraded or touched");
}

void testUnwritable()
{
    test::TempDir dir("unwritable");
    const auto locked = dir.path / "locked";
    std::filesystem::create_directories(locked);
    ::chmod(locked.c_str(), 0500);
    auto store = cs::Store::open(locked / "sub" / "c.sqlite");
    check(!store.has_value() && store.error().kind == cs::Error::Kind::NotWritable,
          "a directory that cannot be made is a named error: " + (store ? std::string("opened") : store.error().message));
    ::chmod(locked.c_str(), 0700);
}

void testRefusedWrites()
{
    test::TempDir dir("refused");
    auto store = cs::Store::open(dir.path / "c.sqlite");
    if (!store) return check(false, "opens");
    auto nan = test::row("lever_arm", "h", std::nan(""));
    check(!store->append(nan).has_value(), "a NaN is never written");
    auto ragged = test::row("lever_arm", "h", 0.3);
    ragged.cov.pop_back();
    check(!store->append(ragged).has_value(), "nor a covariance that is not square");
    auto nameless = test::row("", "h", 0.3);
    check(!store->append(nameless).has_value(), "nor a row with no group");
    const auto h = store->history("lever_arm");
    check(h && h->empty(), "and nothing got in");
    const auto empty = store->latest("lever_arm", "h", 1);
    check(empty && !*empty, "an empty store finds nothing, which is not an error");
}

}  // namespace

int main()
{
    testBadRowsAreSkipped();
    testNotADatabase();
    testNewerSchema();
    testUnwritable();
    testRefusedWrites();
    if (test::failures)
    {
        SPDLOG_ERROR("{} failure(s)", test::failures);
        return 1;
    }
    SPDLOG_INFO("calibration store, malformed: all passed");
    return 0;
}
