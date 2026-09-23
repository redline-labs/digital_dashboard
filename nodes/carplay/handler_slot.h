// SPDX-License-Identifier: GPL-3.0-or-later
//
// A callback that a zenoh thread calls and another thread replaces.
#ifndef CARPLAY_HANDLER_SLOT_H_
#define CARPLAY_HANDLER_SLOT_H_

#include <functional>
#include <mutex>
#include <utility>

namespace carplay
{

// The handler is called WITH the lock held, so once set() returns no call to
// the previous handler is running or can start. That is what makes set(nullptr)
// at teardown safe before destroying whatever the handler captured; copying the
// handler out and calling it unlocked leaves a window where it runs after
// detach.
//
// The price is that nothing may take this lock around something that can call
// back into it on the same thread: a handler must not set() its own slot, and a
// zenoh listener that can fire synchronously at declare time must not be
// declared from inside set().
template <typename... Args>
class HandlerSlot
{
  public:
    using Handler = std::function<void(Args...)>;

    void set(Handler handler)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(handler_, handler);
        }
        // The old handler's captures are destroyed here, outside the lock.
    }

    void operator()(Args... args) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handler_)
        {
            handler_(std::forward<Args>(args)...);
        }
    }

  private:
    mutable std::mutex mutex_;
    Handler handler_;
};

}  // namespace carplay

#endif  // CARPLAY_HANDLER_SLOT_H_
