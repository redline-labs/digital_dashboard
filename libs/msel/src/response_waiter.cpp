// SPDX-License-Identifier: GPL-3.0-or-later

#include "msel/response_waiter.h"

namespace msel
{

void ResponseWaiter::arm()
{
    const std::lock_guard<std::mutex> lock(mMutex);
    mArmed = true;

    // Clearing here rather than in wait() is what makes a stale answer
    // impossible: anything delivered before this moment belongs to an earlier
    // command and is gone by the time this one starts listening.
    mResponse.reset();
}

void ResponseWaiter::deliver(ConfigResponse response)
{
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (!mArmed)
        {
            return;
        }
        mResponse = response;
    }

    // Notified outside the lock so the waking thread does not immediately block
    // on the mutex this one is still holding.
    mSignal.notify_all();
}

std::optional<ConfigResponse> ResponseWaiter::wait(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mMutex);

    // The predicate covers the spurious wakeup and the already-delivered case
    // in one: an answer that arrived between arm() and here is already sitting
    // in mResponse, and wait_for returns immediately.
    mSignal.wait_for(lock, timeout, [this] { return mResponse.has_value(); });

    const std::optional<ConfigResponse> answer = mResponse;
    mArmed = false;
    mResponse.reset();
    return answer;
}

void ResponseWaiter::disarm()
{
    const std::lock_guard<std::mutex> lock(mMutex);
    mArmed = false;
    mResponse.reset();
}

bool ResponseWaiter::armed() const
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mArmed;
}

} // namespace msel
