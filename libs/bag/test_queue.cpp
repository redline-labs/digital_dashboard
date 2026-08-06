// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bounded queue between the zenoh callbacks and the writer thread.
//
// This is where a recorder decides what to lose. A zenoh callback cannot block
// -- stalling an RX thread stalls the whole session -- so when the disk cannot
// keep up there is no backpressure available, only two bad options: grow without
// limit until the process is OOM-killed partway through a recording, or drop and
// say so.
//
// The cases below pin the "and say so" half, because that is the part that is
// easy to get subtly wrong and impossible to notice afterwards: a bag with an
// undercounted drop figure has gaps that read as a publisher having stopped.
//
// The shutdown case matters just as much. A writer loop that runs until pop()
// returns nullopt must NOT be handed nullopt while messages are still queued, or
// pressing Ctrl-C silently discards the tail of every recording.

#include "bag/queue.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

bag::QueuedMessage messageFor(int index)
{
    bag::QueuedMessage message;
    message.key = "vehicle/engine/rpm";
    message.schema = "EngineRpm";
    message.log_time_ns = static_cast<std::uint64_t>(index);
    message.payload.assign(8, static_cast<std::uint8_t>(index & 0xFF));
    return message;
}

// ------------------------------------------------------------------ the cases

void testFifoOrder()
{
    bag::MessageQueue queue(100);

    for (int i = 0; i < 10; ++i)
    {
        expect(queue.push(messageFor(i)), "push " + std::to_string(i) + " did not drop");
    }

    bool ordered = true;
    for (int i = 0; i < 10; ++i)
    {
        const auto message = queue.pop();
        if (!message || message->log_time_ns != static_cast<std::uint64_t>(i))
        {
            ordered = false;
        }
    }
    expect(ordered, "messages come out in the order they went in");
    expect(queue.dropped() == 0, "nothing was dropped");
    expect(queue.depth() == 0, "the queue is empty afterwards");
}

// Over capacity: the queue drops, keeps its bound, and COUNTS.
void testDropsAndCounts()
{
    constexpr std::size_t kCapacity = 10;
    bag::MessageQueue queue(kCapacity);

    std::size_t rejected = 0;
    for (int i = 0; i < 25; ++i)
    {
        if (!queue.push(messageFor(i)))
        {
            ++rejected;
        }
    }

    expect(queue.depth() == kCapacity,
           "the queue never exceeds its capacity (" + std::to_string(queue.depth()) + ")");
    expect(queue.dropped() == 15,
           "every dropped message is counted (" + std::to_string(queue.dropped()) + " of 15)");
    expect(rejected == 15, "and push() reports each drop to its caller");

    // The NEWEST are kept. When a recorder falls behind, the freshest samples
    // are the ones most likely to explain what is happening now -- and a queue
    // that dropped the newest instead would leave the tail of a recording
    // permanently stale.
    const auto first = queue.pop();
    expect(first.has_value() && first->log_time_ns == 15,
           "the oldest are dropped and the newest kept");
}

// pop() must not report "done" while anything is still queued.
void testStopDrainsFirst()
{
    bag::MessageQueue queue(100);

    for (int i = 0; i < 5; ++i)
    {
        queue.push(messageFor(i));
    }
    queue.stop();

    std::size_t drained = 0;
    while (const auto message = queue.pop())
    {
        (void)message;
        ++drained;
    }

    expect(drained == 5,
           "stop() drains what is queued before reporting done -- otherwise Ctrl-C loses the "
           "tail of every recording");
    expect(!queue.pop().has_value(), "and then reports done");
}

// A blocked pop() must wake when stop() is called, or the writer thread never
// joins and the process hangs on exit.
void testStopWakesABlockedPop()
{
    bag::MessageQueue queue(100);

    std::atomic<bool> returned{false};
    std::thread consumer(
        [&]
        {
            const auto message = queue.pop();
            expect(!message.has_value(), "the blocked pop() returns nullopt after stop()");
            returned = true;
        });

    // Let it get into the wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(!returned, "pop() blocks while the queue is empty and not stopped");

    queue.stop();
    consumer.join();
    expect(returned, "stop() wakes a blocked pop()");
}

// The real shape: one producer pushing as fast as it can into a queue too small
// for it, one consumer draining. Nothing may be lost silently -- consumed plus
// dropped must equal produced, exactly.
void testProducerConsumerAccounting()
{
    constexpr int kProduced = 20000;
    constexpr std::size_t kCapacity = 64;

    bag::MessageQueue queue(kCapacity);
    std::atomic<int> consumed{0};

    std::thread consumer(
        [&]
        {
            while (const auto message = queue.pop())
            {
                (void)message;
                ++consumed;
            }
        });

    for (int i = 0; i < kProduced; ++i)
    {
        queue.push(messageFor(i));
    }
    queue.stop();
    consumer.join();

    const std::uint64_t dropped = queue.dropped();
    const int total = consumed.load() + static_cast<int>(dropped);

    expect(total == kProduced,
           "consumed + dropped == produced (" + std::to_string(consumed.load()) + " + " +
               std::to_string(dropped) + " = " + std::to_string(total) + ", expected " +
               std::to_string(kProduced) + ")");
    expect(queue.depth() == 0, "the queue is empty at the end");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testFifoOrder();
    testDropsAndCounts();
    testStopDrainsFirst();
    testStopWakesABlockedPop();
    testProducerConsumerAccounting();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
