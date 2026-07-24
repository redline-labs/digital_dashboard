// SPDX-License-Identifier: GPL-3.0-or-later
//
// NMEA 0183 sentence generation for the accessory -> device location uplink.
//
// CarPlay lets the head unit feed the phone the car's own GPS so the phone can
// dead-reckon where its signal is weak (tunnels, urban canyons). The phone
// requests specific sentence families via StartLocationInformation and we
// answer with LocationInformation messages carrying NMEA strings.
#ifndef IAP2_LOCATION_NMEA_H_
#define IAP2_LOCATION_NMEA_H_

#include <cstdint>
#include <string>

namespace iap2
{

// One GPS fix, in the units the NMEA generators expect. A caller feeds these
// from whatever GPS source the vehicle has.
struct LocationFix
{
    double latitude_deg = 0.0;   // positive north
    double longitude_deg = 0.0;  // positive east
    double altitude_m = 0.0;     // metres above mean sea level
    double speed_knots = 0.0;    // speed over ground
    double course_deg = 0.0;     // true course over ground, 0..360
    uint32_t satellites = 0;     // satellites used in the fix
    double hdop = 1.0;           // horizontal dilution of precision
    uint64_t utc_epoch_ms = 0;   // fix time; 0 uses the current wall clock
    bool valid = true;           // false emits a "no fix" (void) sentence
};

// $GPGGA -- fix data: time, position, quality, satellites, HDOP, altitude.
std::string nmeaGga(const LocationFix& fix);

// $GPRMC -- recommended minimum: time, status, position, speed, course, date.
std::string nmeaRmc(const LocationFix& fix);

// Appends the "*HH\r\n" checksum to a sentence body that starts with '$'.
// Exposed for tests.
std::string appendNmeaChecksum(const std::string& body);

}  // namespace iap2

#endif  // IAP2_LOCATION_NMEA_H_
