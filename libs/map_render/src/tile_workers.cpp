// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_render/tile_workers.h"

#include <algorithm>

namespace map_render
{
namespace
{

// One fewer than the hardware, because the calling thread works too, and capped
// because this is decode and tessellation of at most 64 tiles -- past a handful
// of threads the batch is already short enough that the wake-up costs more than
// the work saved. Zero helpers on a small machine: runAll() must make progress
// with none.
std::size_t defaultHelpers()
{
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 2)
    {
        return 0;
    }
    return std::min<std::size_t>(hardware - 1, 4);
}

} // namespace

TileWorkers::TileWorkers(std::size_t threads)
{
    const std::size_t helpers = threads == 0 ? defaultHelpers() : threads - 1;
    mThreads.reserve(helpers);
    for (std::size_t i = 0; i < helpers; ++i)
    {
        mThreads.emplace_back([this] { workerLoop(); });
    }
}

TileWorkers::~TileWorkers()
{
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mStopping = true;
    }
    mWake.notify_all();
    for (std::thread& thread : mThreads)
    {
        thread.join();
    }
}

void TileWorkers::workerLoop()
{
    std::uint64_t seen = 0;
    while (true)
    {
        const std::function<void(std::size_t)>* job = nullptr;
        std::size_t count = 0;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mWake.wait(lock, [&] { return mStopping || mGeneration != seen; });
            if (mStopping)
            {
                return;
            }
            seen = mGeneration;
            // SNAPSHOT, under the lock. Reading mCount from the claim loop
            // instead is a data race with the next batch's setup, and the
            // symptom is not a wrong count -- it is a straggler claiming an
            // index from the NEXT batch and running it with THIS batch's job
            // pointer, whose captures are already destroyed.
            job = mJob;
            count = mCount;
        }

        if (job != nullptr)
        {
            claim(*job, count);
        }

        // Left the claim loop. runAll() waits for this, not merely for the jobs
        // to finish -- see the header.
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            --mActive;
        }
        mDone.notify_all();
    }
}

void TileWorkers::claim(const std::function<void(std::size_t)>& job, std::size_t count)
{
    // A shared claim counter rather than a static split, because tiles are
    // wildly uneven -- an ocean tile is nothing and a downtown one is thousands
    // of features -- and a static split leaves one thread holding the city
    // while the rest idle.
    while (true)
    {
        const std::size_t index = mNext.fetch_add(1, std::memory_order_relaxed);
        if (index >= count)
        {
            return;
        }
        job(index);
        if (mRemaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mDone.notify_all();
        }
    }
}

void TileWorkers::runAll(std::size_t count, const std::function<void(std::size_t)>& job)
{
    if (count == 0)
    {
        return;
    }

    // A pool with no helpers is a plain loop. Worth short-circuiting rather
    // than paying the condition-variable dance to talk to nobody.
    if (mThreads.empty())
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            job(i);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mJob = &job;
        mCount = count;
        mNext.store(0, std::memory_order_relaxed);
        mRemaining.store(count, std::memory_order_relaxed);
        // Every helper will wake, take what it can, and decrement this on its
        // way out. Set BEFORE the generation bump, so no worker can decrement
        // a count that has not been raised yet.
        mActive = mThreads.size();
        ++mGeneration;
    }
    mWake.notify_all();

    // The caller works too. It is about to block anyway, and on a batch of one
    // this is the whole job.
    claim(job, count);

    {
        std::unique_lock<std::mutex> lock(mMutex);
        // BOTH conditions, and the second is the one that is easy to miss.
        //
        // Waiting only for the jobs to finish lets this return while a helper
        // is still inside its claim loop. The next batch then resets mNext
        // under that helper's feet, and it claims an index belonging to the new
        // batch and runs it through the OLD job -- whose captured references
        // died when the previous runAll() returned. ThreadSanitizer finds this;
        // a test that only counts completed jobs does not.
        mDone.wait(lock, [this] {
            return mRemaining.load(std::memory_order_acquire) == 0 && mActive == 0;
        });
        // Cleared under the lock, with every helper known to be out of the
        // claim loop, so nothing can read a dangling pointer to the caller's
        // job.
        mJob = nullptr;
        mCount = 0;
    }
}

} // namespace map_render
