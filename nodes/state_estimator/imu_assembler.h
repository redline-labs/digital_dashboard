// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pairing the MTi's delta_q and delta_v topics back into one sample.
//
// The bridge publishes each MTData2 item on its own topic, so the two halves
// of one strapdown sample arrive as two messages. Both carry the same packet
// counter in their header, and that -- not arrival order -- is the join: the
// two topics are separate zenoh publications and nothing promises one lands
// before the other. A half whose partner never comes is dropped once the
// counter has moved `patience` samples past it, and counted.
//
// The magnetic field and the pressure come from the same packets, with the
// same header, and ride along: a sample is released once a LATER counter's
// dq and dv are complete (one sample of latency), carrying whatever magnetic
// and pressure items arrived for its own counter. They never hold anything
// up; one that arrives after its sample was released is dropped.
//
// Pure: no zenoh, no capnp.

#ifndef STATE_ESTIMATOR_IMU_ASSEMBLER_H
#define STATE_ESTIMATOR_IMU_ASSEMBLER_H

#include "vehicle_estimator/measurements.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace state_estimator
{

struct ImuHeader
{
    std::uint16_t packetCounter = 0;
    std::uint32_t sampleTimeFine = 0;
};

class ImuAssembler
{
  public:
    explicit ImuAssembler(std::uint16_t patience = 20) : patience_(patience) {}

    void addDeltaQ(const ImuHeader& h, const Eigen::Quaterniond& dq, double arrival);
    void addDeltaV(const ImuHeader& h, const Eigen::Vector3d& dv, double arrival);
    void addMagneticField(const ImuHeader& h, const Eigen::Vector3d& au);
    void addPressure(const ImuHeader& h, double pa);

    // Complete samples, in counter order.
    std::vector<vehicle_estimator::ImuSample> take();

    std::uint64_t orphans() const { return orphans_; }
    std::uint64_t lateAuxiliary() const { return late_aux_; }

  private:
    struct Half
    {
        std::uint16_t counter = 0;
        std::uint32_t tick = 0;
        std::optional<Eigen::Quaterniond> dq;
        std::optional<Eigen::Vector3d> dv;
        std::optional<Eigen::Vector3d> mag;
        std::optional<double> pressure;
        double arrival = 0.0;
    };

    void settle(std::uint16_t newest);

    std::uint16_t patience_;
    // Keyed by counter relative to the first one seen, unwrapped, so a
    // std::map orders them across the UInt16 wrap.
    std::map<std::uint64_t, Half> pending_;
    std::optional<std::uint16_t> last_raw_;
    std::uint64_t unwrapped_ = 0;
    std::vector<vehicle_estimator::ImuSample> ready_;
    std::uint64_t orphans_ = 0;
    std::uint64_t late_aux_ = 0;
    std::optional<std::uint64_t> released_;  // the newest key released
    Half* auxiliary(const ImuHeader& h);

    std::uint64_t unwrap(std::uint16_t counter);
};

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_IMU_ASSEMBLER_H
