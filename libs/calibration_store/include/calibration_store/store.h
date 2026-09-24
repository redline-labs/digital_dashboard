// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the state estimator has learned about the car's installation, kept
// between sessions: a SQLite file of rows, one per group per write, never
// updated and never deleted, so the history of how a mounting or a lever arm
// was learned can be read back with the sqlite3 shell.
//
// Generic on purpose: a row is a group name, the hash of the priors it was
// learned against, a mean and a covariance as numbers. What those numbers
// mean is vehicle_estimator's business, and this library links nothing of
// it. A row whose prior hash no longer matches the config is not an error --
// it simply stops being found, and stays in the history.
//
// Reads are DEFENSIVE. A row that does not parse, is not finite, or whose
// covariance is not symmetric positive definite is skipped, and the next older
// matching row is used: a half-written or hand-edited row must cost one row,
// not the whole calibration. A file that is not a database, or one written by
// a newer schema, is refused and left exactly as it was. A database of ours
// that fails its integrity check is moved aside, kept, and started afresh.
//
// Power loss: WAL with synchronous=FULL. A commit is fsynced before append()
// returns, and a power cut mid-write leaves the last complete commit. That
// holds only if the storage honours the flush -- see
// docs/libs/calibration_store.md.
//
// No zenoh, no Qt, no spdlog: it reports through Result<T>, and the node
// decides what is worth a log line. Same SQLite rule as libs/track_store.
#ifndef CALIBRATION_STORE_STORE_H
#define CALIBRATION_STORE_STORE_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace calibration_store
{

struct Error
{
    enum class Kind
    {
        NotWritable,      // could not create the directory or the file
        NotADatabase,     // exists, and is not SQLite: refused, never truncated
        NewerSchema,      // written by a later version of this library
        Query,
        InvalidArgument,  // a row that must not be written (not finite, wrong shape)
        Corrupt,          // ours, damaged, and could not be moved aside
    };
    Kind kind;
    std::string message;
    int code = 0;
};

template <typename T>
using Result = std::expected<T, Error>;

struct Row
{
    std::int64_t id = 0;             // assigned on append; newest is highest
    std::int64_t written_at_ns = 0;  // wall clock, for a person reading the history
    std::string session;             // which run of the node wrote it
    std::string group;               // "mounting", "lever_arm", ...
    std::string prior_hash;          // hex, from the config the value was learned against
    int model_version = 0;
    std::vector<double> mean;
    std::vector<double> cov;         // n x n, row-major
    std::string summary;             // human-readable; never parsed
    std::string reason;              // why it was written
    double evidence_s = 0.0;         // what it was learned from, where that is counted
    double drive_s = 0.0;            // how long the session had run
};

// What is wrong with a row's numbers, or nothing: a non-empty finite mean,
// and a finite covariance that is square, symmetric and positive definite.
std::optional<std::string> structuralProblem(const Row& row);

inline constexpr int kSchemaVersion = 1;

// A damaged database found at open, and where it was moved.
struct Recovery
{
    std::filesystem::path moved_to;
    std::string reason;
};

class Store
{
  public:
    // Creates the directory and the file if they are not there. A database
    // that fails PRAGMA quick_check is moved to `<path>.corrupt-<unix time>`
    // (with its -wal and -shm) and a fresh one is opened; recovered() says so.
    static Result<Store> open(const std::filesystem::path& path);

    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store();

    // Returns the new row's id.
    Result<std::int64_t> append(const Row& row);
    // Several rows in ONE transaction: all written or none, even across a
    // power cut. Every row is checked before any is written.
    Result<std::vector<std::int64_t>> append(std::span<const Row> rows);

    // Set when open() found this path damaged and started afresh.
    const std::optional<Recovery>& recovered() const;
    // PRAGMA synchronous on this connection: 2 is FULL. For tests and health.
    int synchronousMode() const;

    // The newest row for this group, prior hash and model version that passes
    // the structural checks and `accept` (the caller's own, for what only it
    // knows -- a quaternion's norm). Rows skipped on the way are described in
    // `skipped`. Newest by id, not by written_at_ns: a wall clock can step.
    using Check = std::function<std::optional<std::string>(const Row&)>;
    Result<std::optional<Row>> latest(std::string_view group, std::string_view prior_hash, int model_version,
                                      const Check& accept = {}, std::vector<std::string>* skipped = nullptr) const;

    // Every row of a group, oldest first, whatever its hash; malformed ones
    // included as read, so the history shows what is actually there.
    Result<std::vector<Row>> history(std::string_view group) const;

    const std::filesystem::path& path() const;

  private:
    Store();
    static Result<Store> openOnce(const std::filesystem::path& path, bool may_recover);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace calibration_store

#endif  // CALIBRATION_STORE_STORE_H
