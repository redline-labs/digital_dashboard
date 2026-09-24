#pragma once

// Putting two sensors on one time base without a wire between them.
//
// Each stream has its own clock -- the MTi's 10 kHz tick counter, the
// receiver's GPS time -- and each sample also has a host arrival time. The
// arrival is the device time plus a clock offset plus a latency that varies
// sample to sample but never goes below some floor. So the MINIMUM of
// (host - device) over a window is the clock offset plus that floor, and it
// is stable while any single difference is not. Mapping IMU time into GPS
// time through both minima leaves one unknown: the difference between the
// two floors, which EstimatorConfig::imu_time_offset calibrates.
//
// The mapping slews rather than steps: a new minimum moves it at most
// `max_slew` seconds per second, so an IMU sample is never stamped before
// the one it follows.
//
// This is the naive alignment. A shared PPS into both sensors or into timed
// GPIOs replaces it behind the same interface.

#include <deque>
#include <optional>
#include <utility>

namespace vehicle_estimator
{

class ClockOffset
{
  public:
    explicit ClockOffset(double window = 20.0) : window_(window) {}

    void observe(double device, double host);

    // min(host - device) over the window, once anything has been observed.
    std::optional<double> offset() const;

  private:
    double window_;
    // Monotonic deque of (device time, host - device), increasing offsets.
    std::deque<std::pair<double, double>> mins_;
};

class TimeMapper
{
  public:
    // max_slew: 2 ms per second moves an IMU stamp by 20 us per sample --
    // invisible to the preintegration -- yet follows the few milliseconds
    // the latency minimum settles by in the first seconds of a run.
    TimeMapper(double window = 20.0, double settle = 2.0, double max_slew = 2e-3);

    void observeImu(double device, double host);
    void observeGnss(double gps, double host);

    // IMU device time to GPS time; empty until both streams have been seen
    // for `settle` seconds.
    std::optional<double> imuToGps(double device);

    std::optional<double> currentOffset() const { return mapped_; }

    // Whether the receiver has been heard from at all.
    bool gnssSeen() const { return first_gnss_host_.has_value(); }
    // IMU device time to HOST time, through the IMU's own envelope: the only
    // time base there is before any GNSS, for a start that cannot wait.
    std::optional<double> imuToHost(double device) const
    {
        const auto oi = imu_.offset();
        if (!oi) return std::nullopt;
        return device + *oi;
    }

  private:
    ClockOffset imu_, gnss_;
    double settle_, max_slew_;
    std::optional<double> first_imu_host_, first_gnss_host_;
    std::optional<double> mapped_;  // gps - device, slewed
    std::optional<double> last_device_;
};

}  // namespace vehicle_estimator
