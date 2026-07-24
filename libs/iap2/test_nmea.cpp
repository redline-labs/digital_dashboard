// SPDX-License-Identifier: GPL-3.0-or-later
//
// Verifies the NMEA location sentences against a known fix: field layout,
// ddmm.mmmm coordinate encoding, hemispheres, and the XOR checksum. Runs
// without hardware.
#include "iap2/location_nmea.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>

namespace
{

int g_failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++g_failures;
    }
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Recompute and check the trailing "*HH" checksum of a full sentence.
bool checksumValid(const std::string& sentence)
{
    const size_t star = sentence.rfind('*');
    if (star == std::string::npos || star + 3 > sentence.size())
    {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < star; ++i)
    {
        sum ^= static_cast<uint8_t>(sentence[i]);
    }
    char expected[4];
    std::snprintf(expected, sizeof(expected), "%02X", sum);
    return sentence.compare(star + 1, 2, expected) == 0;
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    // Apple Park, ~1 pm UTC on 2024-01-02, 5 m up, 12.3 kn heading 87.6 deg.
    iap2::LocationFix fix;
    fix.latitude_deg = 37.334900;
    fix.longitude_deg = -122.009020;  // west -> negative
    fix.altitude_m = 5.0;
    fix.speed_knots = 12.3;
    fix.course_deg = 87.6;
    fix.satellites = 9;
    fix.hdop = 0.8;
    fix.utc_epoch_ms = 1704200000000ULL;  // fixed instant, so time fields are stable
    fix.valid = true;

    const std::string gga = iap2::nmeaGga(fix);
    const std::string rmc = iap2::nmeaRmc(fix);
    SPDLOG_INFO("GGA: {}", gga.substr(0, gga.size() - 2));  // trim CRLF for the log
    SPDLOG_INFO("RMC: {}", rmc.substr(0, rmc.size() - 2));

    // Structure.
    expect(gga.rfind("$GPGGA,", 0) == 0, "GGA starts with $GPGGA");
    expect(rmc.rfind("$GPRMC,", 0) == 0, "RMC starts with $GPRMC");
    expect(gga.size() >= 2 && gga.compare(gga.size() - 2, 2, "\r\n") == 0, "GGA ends CRLF");
    expect(rmc.size() >= 2 && rmc.compare(rmc.size() - 2, 2, "\r\n") == 0, "RMC ends CRLF");

    // Coordinates: 37.3349 -> 37 deg 20.0940 min N; 122.00902 -> 122 deg 0.5412 min W.
    expect(contains(gga, "3720.0940,N"), "GGA latitude ddmm.mmmm N");
    expect(contains(gga, "12200.5412,W"), "GGA longitude dddmm.mmmm W");
    expect(contains(rmc, "3720.0940,N"), "RMC latitude");
    expect(contains(rmc, "12200.5412,W"), "RMC longitude");

    // Fields specific to each sentence.
    expect(contains(gga, ",1,09,0.8,5.0,M,"), "GGA quality/sats/hdop/altitude");
    expect(contains(rmc, ",A,"), "RMC status A (valid)");
    expect(contains(rmc, "12.3,87.6,"), "RMC speed and course");

    // Checksums.
    expect(checksumValid(gga), "GGA checksum");
    expect(checksumValid(rmc), "RMC checksum");

    // A void fix flips quality/status and is still well-formed.
    iap2::LocationFix invalid = fix;
    invalid.valid = false;
    const std::string void_gga = iap2::nmeaGga(invalid);
    const std::string void_rmc = iap2::nmeaRmc(invalid);
    expect(contains(void_gga, ",0,"), "void GGA quality 0");
    expect(contains(void_rmc, ",V,"), "void RMC status V");
    expect(checksumValid(void_gga) && checksumValid(void_rmc), "void checksums");

    // Checksum helper on a canonical sentence (NMEA spec example).
    const std::string canonical =
        iap2::appendNmeaChecksum("$GPGLL,4916.45,N,12311.12,W,225444,A");
    expect(contains(canonical, "*31"), "canonical GLL checksum is 31");

    if (g_failures == 0)
    {
        SPDLOG_INFO("nmea tests passed");
        return 0;
    }
    SPDLOG_ERROR("nmea tests FAILED ({} failures)", g_failures);
    return 1;
}
