// Plain value types shared by the parser, the field synthesis and the compiled
// WMMHR2025 interface. Kept free of heavy includes and valid C++20, so the
// public API can be used from code that is not built as C++26.
#pragma once

namespace wmm {

// Gauss coefficients of one (n, m) term. Units are nT and nT/year.
struct Coefficient {
    double g = 0.0;
    double h = 0.0;
    double g_dot = 0.0;
    double h_dot = 0.0;

    constexpr bool operator==(const Coefficient&) const = default;
};

struct Date {
    int year = 0;
    int month = 0;
    int day = 0;

    constexpr bool operator==(const Date&) const = default;
};

constexpr bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr int days_in_month(int year, int month) {
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
}

constexpr bool is_valid(const Date& d) {
    return d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= days_in_month(d.year, d.month);
}

// Calendar date to decimal year, matching MAG_DateToYear: Jan 1 is `year + 0.0`.
constexpr double decimal_year(const Date& d) {
    int day_of_year = d.day - 1;
    for (int month = 1; month < d.month; ++month)
        day_of_year += days_in_month(d.year, month);
    return d.year + day_of_year / (is_leap_year(d.year) ? 366.0 : 365.0);
}

struct GeodeticCoord {
    double latitude_deg = 0.0;   // geodetic latitude, [-90, 90]
    double longitude_deg = 0.0;  // east positive
    double height_km = 0.0;      // above the WGS-84 ellipsoid
};

// Field components in nT (and nT/year for the *_dot rates); angles in degrees.
struct MagneticElements {
    double X = 0, Y = 0, Z = 0;   // north, east, down
    double H = 0, F = 0;          // horizontal intensity, total intensity
    double I = 0, D = 0;          // inclination, declination
    double GV = 0;                // grid variation (polar regions only, NaN elsewhere)
    double X_dot = 0, Y_dot = 0, Z_dot = 0;
    double H_dot = 0, F_dot = 0;
    double I_dot = 0, D_dot = 0, GV_dot = 0;
};

}  // namespace wmm
