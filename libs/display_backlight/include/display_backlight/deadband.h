#ifndef DISPLAY_BACKLIGHT_DEADBAND_H_
#define DISPLAY_BACKLIGHT_DEADBAND_H_

#include "display_backlight/sysfs.h"

#include <optional>
#include <vector>

// Whether a status is worth publishing before the next heartbeat. Sensor noise
// alone should not put a sample on the bus twice a second; a real change should
// not wait for the heartbeat.
namespace display_backlight
{

// ±5 % of the last published lux, but never tighter than ±5 % of 1 lux, so the
// dark end of an opt3001 (0.01 lux steps) does not publish its noise.
inline constexpr double kLuxDeadbandFraction = 0.05;
inline constexpr double kLuxDeadbandFloor = 1.0;
inline constexpr double kTemperatureDeadbandCelsius = 0.5;

// What a status sample says, reduced to what the deadband compares.
struct StatusValues
{
    // nullopt when the backlight is not read at all (not writable).
    std::optional<BacklightStatus> backlight;
    std::vector<std::optional<double>> lux;       // one per light sensor
    std::vector<std::optional<double>> celsius;   // one per temperature channel
};

bool luxMoved(std::optional<double> published, std::optional<double> current);
bool temperatureMoved(std::optional<double> published, std::optional<double> current);

// True when anything moved past its band: any backlight attribute at all, a
// sensor appearing or disappearing, or a different number of sensors.
bool movedPastDeadband(const StatusValues& published, const StatusValues& current);

}  // namespace display_backlight

#endif  // DISPLAY_BACKLIGHT_DEADBAND_H_
