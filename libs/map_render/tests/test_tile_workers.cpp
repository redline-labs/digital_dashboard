// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pool that spreads tile decode across threads.
//
// Worth testing on its own because its one caller cannot be: TileSource needs a
// zenoh session and a server to produce a batch, and a concurrency bug there
// presents as a tile that occasionally does not arrive.

#include "map_render/tile_workers.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <numeric>
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

using map_render::TileWorkers;

// Every index runs, exactly once. A shared claim counter that double-issues an
// index would decode a tile twice; one that skips would lose it silently, which
// is the failure that looks like a slow server.
void test_every_index_runs_exactly_once()
{
    for (std::size_t threads : { std::size_t { 1 }, std::size_t { 2 }, std::size_t { 4 },
                                 std::size_t { 8 } })
    {
        TileWorkers workers(threads);

        constexpr std::size_t kCount = 5000;
        std::vector<std::atomic<int>> visits(kCount);
        for (auto& v : visits)
        {
            v.store(0);
        }

        workers.runAll(kCount, [&](std::size_t i) { visits[i].fetch_add(1); });

        std::size_t wrong = 0;
        for (const auto& v : visits)
        {
            wrong += (v.load() != 1) ? 1 : 0;
        }
        check(wrong == 0, "with " + std::to_string(threads) + " threads every index ran once, " +
                              std::to_string(wrong) + " did not");
    }
}

// runAll() must not return until the last job has finished -- the whole reason
// it blocks is that the caller's data dies when it returns.
void test_run_all_does_not_return_early()
{
    TileWorkers workers(4);

    for (int attempt = 0; attempt < 50; ++attempt)
    {
        std::atomic<int> finished { 0 };
        constexpr std::size_t kCount = 200;

        workers.runAll(kCount, [&](std::size_t i) {
            // Uneven work, like real tiles: an ocean tile is nothing and a
            // downtown one is thousands of features.
            volatile double spin = 0.0;
            for (std::size_t k = 0; k < (i % 32) * 500; ++k)
            {
                spin += double(k);
            }
            (void)spin;
            finished.fetch_add(1);
        });

        if (finished.load() != int(kCount))
        {
            check(false, "runAll returned with " + std::to_string(finished.load()) + " of " +
                             std::to_string(kCount) + " jobs done");
            return;
        }
    }
    check(true, "runAll waits for every job, over 50 rounds");
}

// The pool is reused for every reply, so consecutive batches must not bleed
// into one another.
void test_the_pool_is_reusable()
{
    TileWorkers workers(4);
    std::atomic<int> total { 0 };

    for (int round = 0; round < 200; ++round)
    {
        workers.runAll(20, [&](std::size_t) { total.fetch_add(1); });
    }

    check(total.load() == 200 * 20,
          "200 batches of 20 ran 4000 jobs, got " + std::to_string(total.load()));
}

// A batch of nothing is the common case when every tile is already cached.
void test_an_empty_batch_is_not_a_deadlock()
{
    TileWorkers workers(4);
    workers.runAll(0, [](std::size_t) { check(false, "an empty batch ran a job"); });
    check(true, "an empty batch returns");
}

// A pool with no helpers must degrade to a serial loop rather than to a
// deadlock -- that is what a two-core machine gets.
void test_a_pool_of_one_still_runs_everything()
{
    TileWorkers workers(1);
    check(workers.threadCount() == 1, "a pool of one has one thread: the caller's");

    std::vector<int> order;
    workers.runAll(100, [&](std::size_t i) { order.push_back(int(i)); });

    check(order.size() == 100, "every job ran");
    std::vector<int> expected(100);
    std::iota(expected.begin(), expected.end(), 0);
    check(order == expected, "and in order, because there is nobody to race with");
}

// The bug ThreadSanitizer found, as a behavioural check.
//
// runAll() used to return once the JOBS were done, while a helper could still
// be inside its claim loop. The next batch then reset the shared counter under
// that helper's feet, and it claimed an index belonging to the NEW batch and
// ran it through the OLD job -- whose captured references had already died.
//
// Detected here by tagging each batch: a job that runs for the wrong batch
// shows up as a batch executing more indices than it was given.
void test_a_straggler_cannot_leak_into_the_next_batch()
{
    TileWorkers workers(4);

    constexpr int kBatches = 400;
    constexpr std::size_t kCount = 12;

    std::vector<std::atomic<int>> ran(kBatches);
    for (auto& r : ran)
    {
        r.store(0);
    }

    for (int batch = 0; batch < kBatches; ++batch)
    {
        // Captured by reference on purpose: this is the lifetime the old code
        // could outlive.
        const int thisBatch = batch;
        workers.runAll(kCount, [&ran, thisBatch](std::size_t) {
            ran[std::size_t(thisBatch)].fetch_add(1);
        });
    }

    std::size_t wrong = 0;
    for (int batch = 0; batch < kBatches; ++batch)
    {
        wrong += (ran[std::size_t(batch)].load() != int(kCount)) ? 1 : 0;
    }
    check(wrong == 0, "no batch ran more or fewer jobs than it was given, " +
                          std::to_string(wrong) + " did");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_every_index_runs_exactly_once();
    test_run_all_does_not_return_early();
    test_the_pool_is_reusable();
    test_an_empty_batch_is_not_a_deadlock();
    test_a_pool_of_one_still_runs_everything();
    test_a_straggler_cannot_leak_into_the_next_batch();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all tile worker checks passed");
    return 0;
}
