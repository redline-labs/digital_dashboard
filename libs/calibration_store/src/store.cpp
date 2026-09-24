// SPDX-License-Identifier: GPL-3.0-or-later

#include "calibration_store/store.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cmath>
#include <cstdint>

namespace calibration_store
{

namespace
{

// sqlite3.h is not a SYSTEM include, so its C-cast macros trip
// -Wold-style-cast; same spelling as libs/track_store.
sqlite3_destructor_type sqliteTransient()
{
    return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

std::unexpected<Error> fail(Error::Kind kind, std::string message, int code = 0)
{
    return std::unexpected(Error{kind, std::move(message), code});
}

Result<void> exec(sqlite3* db, const char* sql)
{
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK)
    {
        std::string what = message == nullptr ? "sqlite3_exec failed" : message;
        sqlite3_free(message);
        return fail(rc == SQLITE_NOTADB ? Error::Kind::NotADatabase : Error::Kind::Query, std::move(what), rc);
    }
    return {};
}

// A prepared statement that finalises itself.
class Statement
{
  public:
    Statement(sqlite3* db, const char* sql) { rc_ = sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    ~Statement() { sqlite3_finalize(s_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    bool ok() const { return rc_ == SQLITE_OK; }
    int rc() const { return rc_; }
    sqlite3_stmt* get() const { return s_; }

  private:
    sqlite3_stmt* s_ = nullptr;
    int rc_ = SQLITE_OK;
};

std::string text(sqlite3_stmt* s, int column)
{
    const auto* t = sqlite3_column_text(s, column);
    return t == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(t));
}

void bindText(sqlite3_stmt* s, int index, std::string_view value)
{
    sqlite3_bind_text(s, index, value.data(), static_cast<int>(value.size()), sqliteTransient());
}

std::string toJson(const std::vector<double>& v)
{
    return nlohmann::json(v).dump();
}

// A JSON array of numbers, or nothing. Anything else -- an object, a string,
// a null where NaN was written -- is a malformed row.
std::optional<std::vector<double>> fromJson(const std::string& s)
{
    const auto j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return std::nullopt;
    std::vector<double> out;
    out.reserve(j.size());
    for (const auto& x : j)
    {
        if (!x.is_number()) return std::nullopt;
        out.push_back(x.get<double>());
    }
    return out;
}

constexpr const char* kColumns =
    "id, written_at_ns, session, grp, prior_hash, model_version, mean, cov, summary, reason, evidence_s, drive_s";

// Reads one row. The numbers columns are parsed here; a parse failure leaves
// them empty and is reported in `problem`.
Row readRow(sqlite3_stmt* s, std::optional<std::string>& problem)
{
    Row r;
    r.id = sqlite3_column_int64(s, 0);
    r.written_at_ns = sqlite3_column_int64(s, 1);
    r.session = text(s, 2);
    r.group = text(s, 3);
    r.prior_hash = text(s, 4);
    r.model_version = sqlite3_column_int(s, 5);
    const auto mean = fromJson(text(s, 6));
    const auto cov = fromJson(text(s, 7));
    if (!mean || !cov)
        problem = "row " + std::to_string(r.id) + ": mean or covariance is not a JSON array of numbers";
    else
    {
        r.mean = *mean;
        r.cov = *cov;
    }
    r.summary = text(s, 8);
    r.reason = text(s, 9);
    r.evidence_s = sqlite3_column_double(s, 10);
    r.drive_s = sqlite3_column_double(s, 11);
    return r;
}

}  // namespace

std::optional<std::string> structuralProblem(const Row& row)
{
    if (row.mean.empty()) return "empty mean";
    for (double x : row.mean)
        if (!std::isfinite(x)) return "mean is not finite";
    const auto n = static_cast<std::size_t>(std::llround(std::sqrt(static_cast<double>(row.cov.size()))));
    if (n == 0 || n * n != row.cov.size()) return "covariance is not square";
    for (double x : row.cov)
        if (!std::isfinite(x)) return "covariance is not finite";
    double scale = 0.0;
    for (double x : row.cov) scale = std::max(scale, std::fabs(x));
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (std::fabs(row.cov[i * n + j] - row.cov[j * n + i]) > 1e-9 * (1.0 + scale))
                return "covariance is not symmetric";
    // Cholesky, in place on a copy: fails exactly when not positive definite.
    std::vector<double> L(row.cov);
    for (std::size_t j = 0; j < n; ++j)
    {
        double d = L[j * n + j];
        for (std::size_t k = 0; k < j; ++k) d -= L[j * n + k] * L[j * n + k];
        if (!(d > 0.0)) return "covariance is not positive definite";
        d = std::sqrt(d);
        L[j * n + j] = d;
        for (std::size_t i = j + 1; i < n; ++i)
        {
            double v = L[i * n + j];
            for (std::size_t k = 0; k < j; ++k) v -= L[i * n + k] * L[j * n + k];
            L[i * n + j] = v / d;
        }
    }
    return std::nullopt;
}

struct Store::Impl
{
    sqlite3* db = nullptr;
    std::filesystem::path path;
    ~Impl()
    {
        if (db != nullptr) sqlite3_close(db);
    }
};

Store::Store() : impl_(std::make_unique<Impl>()) {}
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

const std::filesystem::path& Store::path() const
{
    return impl_->path;
}

Result<Store> Store::open(const std::filesystem::path& path)
{
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(Error::Kind::NotWritable, path.parent_path().string() + ": " + ec.message());

    Store store;
    store.impl_->path = path;
    const int rc = sqlite3_open_v2(path.string().c_str(), &store.impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                   nullptr);
    if (rc != SQLITE_OK) return fail(Error::Kind::NotWritable, path.string(), rc);
    sqlite3* db = store.impl_->db;

    // Nothing is written until the file is known to be ours and not newer.
    // SQLite itself refuses to write to a file whose header is not SQLite's
    // (the first statement fails with SQLITE_NOTADB), so a file that is not a
    // database is refused before and without any write.
    {
        Statement version(db, "SELECT value FROM meta WHERE name='schema_version'");
        if (!version.ok() && version.rc() == SQLITE_NOTADB)
            return fail(Error::Kind::NotADatabase, path.string() + ": " + sqlite3_errmsg(db), version.rc());
        if (version.ok() && sqlite3_step(version.get()) == SQLITE_ROW)
        {
            const int v = std::atoi(text(version.get(), 0).c_str());
            if (v > kSchemaVersion)
                return fail(Error::Kind::NewerSchema,
                            path.string() + " has schema " + std::to_string(v) + "; this reads up to " +
                                std::to_string(kSchemaVersion));
        }
    }

    // WAL: a crash mid-write loses that write, not the file. NORMAL is safe
    // under WAL against a crash; a power cut may lose the last write, which the
    // next session's writes put back.
    if (auto r = exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"); !r) return std::unexpected(r.error());
    if (auto r = exec(db, R"(
        BEGIN;
        CREATE TABLE IF NOT EXISTS meta(name TEXT PRIMARY KEY, value TEXT NOT NULL);
        INSERT OR IGNORE INTO meta VALUES('schema_version', '1');
        CREATE TABLE IF NOT EXISTS calibration(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            written_at_ns INTEGER NOT NULL,
            session TEXT NOT NULL,
            grp TEXT NOT NULL,
            prior_hash TEXT NOT NULL,
            model_version INTEGER NOT NULL,
            mean TEXT NOT NULL,
            cov TEXT NOT NULL,
            summary TEXT NOT NULL,
            reason TEXT NOT NULL,
            evidence_s REAL NOT NULL,
            drive_s REAL NOT NULL);
        CREATE INDEX IF NOT EXISTS calibration_lookup ON calibration(grp, prior_hash, model_version, id);
        CREATE VIEW IF NOT EXISTS calibration_latest AS
            SELECT c.* FROM calibration c
            JOIN (SELECT MAX(id) AS id FROM calibration GROUP BY grp, prior_hash, model_version) m ON c.id = m.id;
        COMMIT;)");
        !r)
    {
        exec(db, "ROLLBACK");
        return std::unexpected(r.error());
    }
    return store;
}

Result<std::int64_t> Store::append(const Row& row)
{
    if (auto p = structuralProblem(row)) return fail(Error::Kind::InvalidArgument, *p);
    if (row.group.empty() || row.prior_hash.empty())
        return fail(Error::Kind::InvalidArgument, "a row needs a group and a prior hash");
    Statement s(impl_->db, "INSERT INTO calibration(written_at_ns, session, grp, prior_hash, model_version, mean, cov, "
                           "summary, reason, evidence_s, drive_s) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    if (!s.ok()) return fail(Error::Kind::Query, sqlite3_errmsg(impl_->db), s.rc());
    sqlite3_bind_int64(s.get(), 1, row.written_at_ns);
    bindText(s.get(), 2, row.session);
    bindText(s.get(), 3, row.group);
    bindText(s.get(), 4, row.prior_hash);
    sqlite3_bind_int(s.get(), 5, row.model_version);
    bindText(s.get(), 6, toJson(row.mean));
    bindText(s.get(), 7, toJson(row.cov));
    bindText(s.get(), 8, row.summary);
    bindText(s.get(), 9, row.reason);
    sqlite3_bind_double(s.get(), 10, row.evidence_s);
    sqlite3_bind_double(s.get(), 11, row.drive_s);
    const int rc = sqlite3_step(s.get());
    if (rc != SQLITE_DONE) return fail(Error::Kind::Query, sqlite3_errmsg(impl_->db), rc);
    return sqlite3_last_insert_rowid(impl_->db);
}

Result<std::optional<Row>> Store::latest(std::string_view group, std::string_view prior_hash, int model_version,
                                         const Check& accept, std::vector<std::string>* skipped) const
{
    const std::string sql = std::string("SELECT ") + kColumns +
                            " FROM calibration WHERE grp=? AND prior_hash=? AND model_version=? ORDER BY id DESC";
    Statement s(impl_->db, sql.c_str());
    if (!s.ok()) return fail(Error::Kind::Query, sqlite3_errmsg(impl_->db), s.rc());
    bindText(s.get(), 1, group);
    bindText(s.get(), 2, prior_hash);
    sqlite3_bind_int(s.get(), 3, model_version);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW)
    {
        std::optional<std::string> problem;
        Row r = readRow(s.get(), problem);
        if (!problem)
            if (auto p = structuralProblem(r)) problem = "row " + std::to_string(r.id) + ": " + *p;
        if (!problem && accept)
            if (auto p = accept(r)) problem = "row " + std::to_string(r.id) + ": " + *p;
        if (!problem) return r;
        if (skipped) skipped->push_back(*problem);
    }
    if (rc != SQLITE_DONE) return fail(Error::Kind::Query, sqlite3_errmsg(impl_->db), rc);
    return std::optional<Row>{};
}

Result<std::vector<Row>> Store::history(std::string_view group) const
{
    const std::string sql = std::string("SELECT ") + kColumns + " FROM calibration WHERE grp=? ORDER BY id";
    Statement s(impl_->db, sql.c_str());
    if (!s.ok()) return fail(Error::Kind::Query, sqlite3_errmsg(impl_->db), s.rc());
    bindText(s.get(), 1, group);
    std::vector<Row> out;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW)
    {
        std::optional<std::string> problem;
        out.push_back(readRow(s.get(), problem));
    }
    if (rc != SQLITE_DONE) return fail(Error::Kind::Query, sqlite3_errmsg(impl_->db), rc);
    return out;
}

}  // namespace calibration_store
