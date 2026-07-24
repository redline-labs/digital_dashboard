// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2/location_nmea.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace iap2
{
namespace
{

// UTC broken-down time for the fix (its own timestamp, or now if unset).
std::tm fixUtc(const LocationFix& fix)
{
    const std::time_t seconds =
        fix.utc_epoch_ms != 0 ? static_cast<std::time_t>(fix.utc_epoch_ms / 1000) : std::time(nullptr);
    std::tm out{};
    gmtime_r(&seconds, &out);
    return out;
}

// NMEA latitude/longitude are ddmm.mmmm (degrees, then decimal minutes) plus a
// hemisphere letter. `is_lat` selects the field width (2 vs 3 degree digits).
std::string degreesMinutes(double value, bool is_lat, char positive, char negative)
{
    const char hemisphere = value >= 0.0 ? positive : negative;
    value = std::fabs(value);
    const int degrees = static_cast<int>(value);
    const double minutes = (value - degrees) * 60.0;

    char buf[24];
    if (is_lat)
    {
        std::snprintf(buf, sizeof(buf), "%02d%07.4f,%c", degrees, minutes, hemisphere);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%03d%07.4f,%c", degrees, minutes, hemisphere);
    }
    return buf;
}

}  // namespace

std::string appendNmeaChecksum(const std::string& body)
{
    // XOR of every byte between '$' and '*'.
    uint8_t sum = 0;
    for (size_t i = 1; i < body.size(); ++i)
    {
        sum ^= static_cast<uint8_t>(body[i]);
    }
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    return body + tail;
}

std::string nmeaGga(const LocationFix& fix)
{
    const std::tm utc = fixUtc(fix);
    char time_field[16];
    std::snprintf(time_field, sizeof(time_field), "%02d%02d%02d.00", utc.tm_hour, utc.tm_min,
                  utc.tm_sec);

    std::string body = "$GPGGA,";
    body += time_field;
    body += ",";
    body += degreesMinutes(fix.latitude_deg, true, 'N', 'S');
    body += ",";
    body += degreesMinutes(fix.longitude_deg, false, 'E', 'W');
    body += ",";

    char rest[64];
    // Fix quality: 1 = GPS fix, 0 = invalid.
    std::snprintf(rest, sizeof(rest), "%d,%02u,%.1f,%.1f,M,0.0,M,,", fix.valid ? 1 : 0,
                  fix.satellites, fix.hdop, fix.altitude_m);
    body += rest;

    return appendNmeaChecksum(body);
}

std::string nmeaRmc(const LocationFix& fix)
{
    const std::tm utc = fixUtc(fix);
    char time_field[16];
    std::snprintf(time_field, sizeof(time_field), "%02d%02d%02d.00", utc.tm_hour, utc.tm_min,
                  utc.tm_sec);
    char date_field[8];
    std::snprintf(date_field, sizeof(date_field), "%02d%02d%02d", utc.tm_mday, utc.tm_mon + 1,
                  (utc.tm_year + 1900) % 100);

    std::string body = "$GPRMC,";
    body += time_field;
    body += fix.valid ? ",A," : ",V,";  // A = valid, V = warning/no fix
    body += degreesMinutes(fix.latitude_deg, true, 'N', 'S');
    body += ",";
    body += degreesMinutes(fix.longitude_deg, false, 'E', 'W');
    body += ",";

    char rest[48];
    std::snprintf(rest, sizeof(rest), "%.1f,%.1f,", fix.speed_knots, fix.course_deg);
    body += rest;
    body += date_field;
    body += ",,,A";  // empty magnetic variation, mode A (autonomous)

    return appendNmeaChecksum(body);
}

}  // namespace iap2
