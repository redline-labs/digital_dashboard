// The transmit queue between the tx topic's callback and the adapter: push
// never blocks, a full queue refuses the newest frame and counts it, order is
// kept, and close() releases the sender.

#include "can_zenoh/tx_queue.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

namespace
{

using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

helpers::CanFrame frameWithId(uint32_t id)
{
    helpers::CanFrame frame;
    frame.id = id;
    return frame;
}

void overflowRefusesTheNewest()
{
    can_zenoh::TxQueue queue(3);
    for (uint32_t id = 1; id <= 5; ++id)
    {
        const bool queued = queue.push(frameWithId(id));
        expect(queued == (id <= 3), "frame " + std::to_string(id) + " queued only if there was room");
    }
    expect(queue.size() == 3, "the queue holds its depth");
    expect(queue.dropped() == 2, "and counts what it refused");

    for (uint32_t id = 1; id <= 3; ++id)
    {
        const auto frame = queue.pop();
        expect(frame.has_value() && frame->id == id,
               "the oldest frames come out first, in order: " + std::to_string(id));
    }
    expect(queue.push(frameWithId(6)), "room again once drained");
}

void closeReleasesAWaitingSender()
{
    can_zenoh::TxQueue queue(4);
    auto waiting = std::async(std::launch::async, [&] { return queue.pop(); });
    expect(waiting.wait_for(50ms) == std::future_status::timeout, "pop waits on an empty queue");

    queue.push(frameWithId(7));
    expect(waiting.wait_for(1s) == std::future_status::ready, "and wakes for a frame");
    const auto frame = waiting.get();
    expect(frame.has_value() && frame->id == 7, "the frame pushed");

    auto parked = std::async(std::launch::async, [&] { return queue.pop(); });
    std::this_thread::sleep_for(20ms);
    queue.close();
    expect(parked.wait_for(1s) == std::future_status::ready, "close releases a waiting pop");
    expect(!parked.get().has_value(), "with nothing");
    expect(!queue.push(frameWithId(8)), "a closed queue refuses frames");
    expect(queue.dropped() == 0, "without counting shutdown as a drop");

    queue.reopen();
    expect(queue.push(frameWithId(9)), "reopened, it takes frames again");
}

}  // namespace

int main()
{
    overflowRefusesTheNewest();
    closeReleasesAWaitingSender();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
