#include "vehicle_estimator/clock.h"

#include <algorithm>
#include <cmath>

namespace vehicle_estimator
{

void ClockOffset::observe(double device, double host)
{
    if (!std::isfinite(device) || !std::isfinite(host)) return;
    const double off = host - device;
    // Anything behind the new value with a larger offset can never be the
    // minimum again: it leaves the window first.
    while (!mins_.empty() && mins_.back().second >= off) mins_.pop_back();
    mins_.emplace_back(device, off);
    while (!mins_.empty() && mins_.front().first < device - window_) mins_.pop_front();
}

std::optional<double> ClockOffset::offset() const
{
    if (mins_.empty()) return std::nullopt;
    return mins_.front().second;
}

TimeMapper::TimeMapper(double window, double settle, double max_slew)
    : imu_(window), gnss_(window), settle_(settle), max_slew_(max_slew)
{
}

void TimeMapper::observeImu(double device, double host)
{
    imu_.observe(device, host);
    if (!first_imu_host_) first_imu_host_ = host;
}

void TimeMapper::observeGnss(double gps, double host)
{
    gnss_.observe(gps, host);
    if (!first_gnss_host_) first_gnss_host_ = host;
}

std::optional<double> TimeMapper::imuToGps(double device)
{
    const auto oi = imu_.offset(), og = gnss_.offset();
    if (!oi || !og || !first_imu_host_ || !first_gnss_host_) return std::nullopt;
    const double now = device + *oi;  // host time, roughly
    if (now - std::max(*first_imu_host_, *first_gnss_host_) < settle_) return std::nullopt;

    // gps = host - og = device + oi - og
    const double target = *oi - *og;
    if (!mapped_)
    {
        mapped_ = target;
    }
    else if (last_device_)
    {
        const double step = std::max(0.0, device - *last_device_) * max_slew_;
        mapped_ = *mapped_ + std::clamp(target - *mapped_, -step, step);
    }
    last_device_ = device;
    return device + *mapped_;
}

}  // namespace vehicle_estimator
