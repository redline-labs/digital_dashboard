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
#include <vector>

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

void testDamagedDatabaseIsMovedAside()
{
    // Ours, damaged: what a power cut does on storage that acknowledged a
    // flush it had not made. Refusing it would leave the car on its config
    // until someone came; instead it is moved aside, kept, and begun again.
    test::TempDir dir("damaged");
    const auto file = dir.path / "c.sqlite";
    {
        auto store = cs::Store::open(file);
        if (!store) return check(false, "opens");
        for (int i = 0; i < 200; ++i) store->append(test::row("lever_arm", "h", 0.3 + 1e-4 * i));
    }
    // Everything into the main file; then more rows committed to the WAL and
    // left there, as a power cut would leave them; then the calibration
    // table's root page scribbled over. Page one -- the header and schema --
    // survives, so it still reads as ours. (Scribbling over free space inside
    // a page is not damage: quick_check rightly says ok.)
    sqlite3* db = nullptr;
    sqlite3_open(file.string().c_str(), &db);
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    long root = 0, page_size = 0;
    {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_master WHERE name='calibration'", -1, &s, nullptr);
        if (sqlite3_step(s) == SQLITE_ROW) root = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &s, nullptr);
        if (sqlite3_step(s) == SQLITE_ROW) page_size = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
    sqlite3_exec(db, "INSERT INTO meta VALUES('left_in_the_wal', 'yes');", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    check(root > 1 && page_size > 0, "found the table's root page");
    check(std::filesystem::exists(file.string() + "-wal") && std::filesystem::file_size(file.string() + "-wal") > 0,
          "a committed transaction is waiting in the WAL");
    const std::string wal_before = readAll(file.string() + "-wal");
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp((root - 1) * page_size);
        const std::string junk(static_cast<std::size_t>(page_size), '\x5a');
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    const std::string damaged = readAll(file);

    auto store = cs::Store::open(file);
    check(store.has_value(), "a damaged store still opens");
    if (!store) return;
    check(store->recovered().has_value(), "saying it recovered");
    if (!store->recovered()) return;
    const auto& rec = *store->recovered();
    SPDLOG_INFO("recovered: moved to {} because {}", rec.moved_to.string(), rec.reason);
    check(std::filesystem::exists(rec.moved_to) && readAll(rec.moved_to) == damaged,
          "the damaged file is kept, byte for byte, where it says");
    check(std::filesystem::exists(rec.moved_to.string() + "-wal") && readAll(rec.moved_to.string() + "-wal") == wal_before,
          "its log moved with it, unapplied: nothing was checkpointed into the damaged file");
    const auto h = store->history("lever_arm");
    check(h && h->empty(), "the new store starts empty");
    check(store->append(test::row("lever_arm", "h", 0.3)).has_value(), "and takes writes");

    auto again = cs::Store::open(file);
    check(again && !again->recovered(), "the store it started is not itself treated as damaged");
}

void testBatchFailingMidwayWritesNothing()
{
    // A batch that passes every check and then fails in SQLite on its second
    // row -- here a trigger, on the car a full disk or an I/O error. The
    // first row must not survive alone: one transaction or none.
    test::TempDir dir("midway");
    const auto file = dir.path / "c.sqlite";
    {
        auto store = cs::Store::open(file);
        if (!store) return check(false, "opens");
    }
    sqlite3* db = nullptr;
    sqlite3_open(file.string().c_str(), &db);
    sqlite3_exec(db,
                 "CREATE TRIGGER boom BEFORE INSERT ON calibration WHEN NEW.grp = 'boresight' "
                 "BEGIN SELECT RAISE(ABORT, 'no room'); END;",
                 nullptr, nullptr, nullptr);
    sqlite3_close(db);
    auto store = cs::Store::open(file);
    if (!store) return check(false, "reopens");
    const std::vector<cs::Row> batch{test::row("mounting", "h", 0.1), test::row("boresight", "h", 0.2)};
    const auto ids = store->append(std::span<const cs::Row>(batch));
    check(!ids.has_value(), "the batch fails");
    const auto h = store->history("mounting");
    check(h && h->empty(), "and the row before the failure was rolled back with it");
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
    testDamagedDatabaseIsMovedAside();
    testBatchFailingMidwayWritesNothing();
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
