// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_LOCATION_CACHE_H_
#define CARPLAY_LOCATION_CACHE_H_

#include "location_fix.h"

#include <chrono>
#include <mutex>
#include <optional>

namespace carplay
{

// The latest GPS fix, and whether it is still a fix.
//
// The phone is sent a position once a second for as long as it asks. Without
// an age, a GPS source that stopped publishing left the phone receiving the
// last position forever, marked valid and stamped with the current time --
// a car parked on the map while it drives. Past `stale_after` the fix goes out
// as "no fix" instead, which the phone dead-reckons through. A fix that carries
// no time of its own is stamped with when it arrived, not when it is sent.
class LocationCache
{
  public:
    using steady = std::chrono::steady_clock;
    using system = std::chrono::system_clock;

    explicit LocationCache(std::chrono::milliseconds stale_after) : stale_after_(stale_after) {}

    void update(LocationFix fix, steady::time_point arrived, system::time_point arrived_wall)
    {
        if (fix.utc_epoch_ms == 0)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                arrived_wall.time_since_epoch());
            fix.utc_epoch_ms = static_cast<uint64_t>(ms.count());
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        fix_ = fix;
        arrived_ = arrived;
    }

    // A fix that never ages: the bench's --location.
    void pin(const LocationFix& fix)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        fix_ = fix;
        pinned_ = true;
    }

    // nullopt until a fix has arrived; then the fix, invalid once it is stale.
    std::optional<LocationFix> current(steady::time_point now) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!fix_)
        {
            return std::nullopt;
        }
        LocationFix out = *fix_;
        if (!pinned_ && now - arrived_ > stale_after_)
        {
            out.valid = false;
        }
        return out;
    }

  private:
    const std::chrono::milliseconds stale_after_;
    mutable std::mutex mutex_;
    std::optional<LocationFix> fix_;
    steady::time_point arrived_{};
    bool pinned_ = false;
};

}  // namespace carplay

#endif  // CARPLAY_LOCATION_CACHE_H_
