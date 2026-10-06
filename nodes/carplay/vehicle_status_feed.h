// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_VEHICLE_STATUS_FEED_H_
#define CARPLAY_VEHICLE_STATUS_FEED_H_

#include "vehicle_status.h"

#include <mutex>
#include <optional>

namespace carplay
{

// The vehicle status the phone is told, and when to tell it again.
//
// The config's values are the starting point; a live source on the bus
// replaces them key by key, so a publisher that knows only the range does not
// erase a configured temperature. While the phone is subscribed, a change is
// sent once -- not on every poll, and not when nothing moved. A range fixed in
// config is wrong a kilometre later, which is why the live half exists.
//
// apply() runs on a zenoh thread and takeChange() on the iAP2 one.
class VehicleStatusFeed
{
  public:
    VehicleStatusFeed(VehicleStatus baseline, bool live) : current_(baseline), live_(live) {}

    // Whether to declare a VehicleStatusComponent at all: only if there is,
    // or will be, something to answer the phone's subscription with.
    bool advertised() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return live_ || current_.any();
    }

    // A live update. Absent fields leave the current value alone.
    void apply(const VehicleStatus& update)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (update.range_km)
        {
            current_.range_km = update.range_km;
        }
        if (update.outside_temperature_c)
        {
            current_.outside_temperature_c = update.outside_temperature_c;
        }
        if (update.range_warning)
        {
            current_.range_warning = update.range_warning;
        }
    }

    // The phone subscribed: it wants the current values now, whatever was
    // sent before.
    void subscribed()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sent_.reset();
    }

    // What to send, if anything differs from what the phone was last sent.
    std::optional<VehicleStatus> takeChange()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!current_.any() || (sent_ && *sent_ == current_))
        {
            return std::nullopt;
        }
        sent_ = current_;
        return current_;
    }

    VehicleStatus current() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

  private:
    mutable std::mutex mutex_;
    VehicleStatus current_;
    std::optional<VehicleStatus> sent_;
    bool live_;
};

}  // namespace carplay

#endif  // CARPLAY_VEHICLE_STATUS_FEED_H_
