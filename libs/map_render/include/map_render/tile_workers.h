// SPDX-License-Identifier: GPL-3.0-or-later
//
// A fixed pool of threads, and one thing to do with it: run an indexed job
// across them and wait.
//
// It exists for one caller. A tile reply carries up to 64 tiles, and turning
// each into triangles was measured at ~1.9 ms -- so a batch is a fifth of a
// second of decode and tessellation, and it all ran on the single zenoh thread
// the reply arrived on. That does not block painting, but it does decide how
// long a pan takes to fill in.
//
// WHY A BLOCKING PARALLEL-FOR rather than a queue the callback posts into and
// forgets. The tile bytes are a `std::span` into the capnp reply message, which
// is alive only for the duration of the callback. Handing that span to a worker
// that outlives the callback is a use-after-free; handing it a COPY is 64
// memcpys of a few hundred kilobytes per batch. Blocking until every worker is
// done keeps the span valid and copies nothing -- and the thread being blocked
// is the one that was going to do all this work serially anyway.
#ifndef MAP_TILE_WORKERS_H
#define MAP_TILE_WORKERS_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace map_render
{

class TileWorkers
{
  public:
    // Threads are created up front and live until the pool does. `threads` of 0
    // asks for a sensible default from the hardware.
    explicit TileWorkers(std::size_t threads = 0);
    ~TileWorkers();

    TileWorkers(const TileWorkers&) = delete;
    TileWorkers& operator=(const TileWorkers&) = delete;

    // Run `job(i)` for every i in [0, count) and return once all of them have
    // finished. The calling thread takes work too, so a pool of one degrades to
    // a plain serial loop rather than to a deadlock.
    //
    // `job` runs on several threads at once and must not assume otherwise.
    // An exception escaping it terminates: there is no per-item result to carry
    // one back through, and the caller has nothing useful to do with it.
    void runAll(std::size_t count, const std::function<void(std::size_t)>& job);

    std::size_t threadCount() const { return mThreads.size() + 1; }

  private:
    void workerLoop();
    // Take indices off the shared counter until there are none left. `count` is
    // passed by value, never read from the member: reading the member here
    // races with the next batch's setup.
    void claim(const std::function<void(std::size_t)>& job, std::size_t count);

    std::vector<std::thread> mThreads;

    std::mutex mMutex;
    std::condition_variable mWake;
    // Bumped once per runAll(), so a worker can tell a new job from a spurious
    // wake without comparing function pointers.
    std::uint64_t mGeneration { 0 };
    const std::function<void(std::size_t)>* mJob { nullptr };
    std::size_t mCount { 0 };
    std::atomic<std::size_t> mNext { 0 };
    std::atomic<std::size_t> mRemaining { 0 };
    // Helpers that have woken for this batch and not yet left the claim loop.
    // runAll() waits for this to reach zero as well as for the jobs to finish;
    // without it a straggler leaks into the next batch. Guarded by mMutex.
    std::size_t mActive { 0 };
    std::condition_variable mDone;
    bool mStopping { false };
};

} // namespace map_render

#endif // MAP_TILE_WORKERS_H
