// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CALIBRATION_STORE_TESTS_COMMON_H
#define CALIBRATION_STORE_TESTS_COMMON_H

#include "calibration_store/store.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <unistd.h>

namespace test
{

inline int failures = 0;

inline void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// A directory of its own, removed afterwards: the store makes -wal and -shm
// files beside the database.
struct TempDir
{
    std::filesystem::path path;
    explicit TempDir(const std::string& label)
        : path(std::filesystem::temp_directory_path() /
               ("calibration_store_test_" + label + "_" + std::to_string(::getpid())))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

inline calibration_store::Row row(const std::string& group, const std::string& hash, double x, int version = 1)
{
    calibration_store::Row r;
    r.written_at_ns = 1790000000000000000;
    r.session = "test";
    r.group = group;
    r.prior_hash = hash;
    r.model_version = version;
    r.mean = {x, 0.0, 1.2};
    r.cov = {1e-4, 0.0, 0.0, 0.0, 1e-4, 0.0, 0.0, 0.0, 2.5e-3};
    r.summary = "x " + std::to_string(x);
    r.reason = "first_converged";
    r.evidence_s = 12.5;
    r.drive_s = 600.0;
    return r;
}

}  // namespace test

#endif
