// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_VEHICLE_STATUS_H_
#define CARPLAY_VEHICLE_STATUS_H_

#include <cstdint>
#include <optional>

namespace carplay
{

// What the vehicle reports about itself while a session is live.
//
// The phone subscribes to these with StartVehicleStatusUpdates and CarPlay
// surfaces them (range in the Maps trip planner, outside temperature in the
// status area). Every field is optional and nothing is invented: an unset field
// is left out of the update, and with all of them unset and no live source the
// capability is not advertised at all -- declaring a VehicleStatusComponent and
// then never answering the phone's subscription is a promise broken on every
// session.
//
// From the config, from a live source on the bus, or both: see
// VehicleIdentity::status_live and VehicleStatusFeed.
struct VehicleStatus
{
    // Remaining driving range, in kilometres.
    std::optional<uint16_t> range_km;
    // Outside air temperature, in degrees Celsius.
    std::optional<int16_t> outside_temperature_c;
    // True when the range is low enough that CarPlay should say so.
    std::optional<bool> range_warning;

    // Whether there is anything to report at all.
    bool any() const
    {
        return range_km.has_value() || outside_temperature_c.has_value() ||
               range_warning.has_value();
    }

    bool operator==(const VehicleStatus&) const = default;
};

}  // namespace carplay

#endif  // CARPLAY_VEHICLE_STATUS_H_
