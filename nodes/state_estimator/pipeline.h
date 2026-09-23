// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bus messages in, vehicle states out -- with no bus.
//
// The live node feeds this from its subscriptions, --replay and the offline
// tool feed it from a bag, and the tests feed it capnp messages built from a
// simulated drive. All three therefore run the same decode, the same
// pairing, and the same estimator; only where the bytes come from differs.

#ifndef STATE_ESTIMATOR_PIPELINE_H
#define STATE_ESTIMATOR_PIPELINE_H

#include "decode.h"
#include "gnss_assembler.h"
#include "imu_assembler.h"
#include "node_config.h"

#include "vehicle_estimator/estimator.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace state_estimator
{

class Pipeline
{
  public:
    explicit Pipeline(const NodeConfig& config);

    // One message, as it arrived. `arrival` in seconds on any clock that is
    // the same for every message.
    Fed onMessage(std::string_view schema, std::span<const std::uint8_t> payload, double arrival);

    // Moves whatever the assemblers have completed by `now` into the
    // estimator, in arrival order, and returns one state per IMU sample
    // processed once the estimator is running.
    std::vector<vehicle_estimator::VehicleState> advance(double now);

    vehicle_estimator::Estimator& estimator() { return estimator_; }
    const vehicle_estimator::Estimator& estimator() const { return estimator_; }
    const GnssAssembler& gnss() const { return gnss_; }
    const ImuAssembler& imu() const { return imu_; }

    std::uint64_t malformed() const { return malformed_; }

  private:
    GnssAssembler gnss_;
    ImuAssembler imu_;
    vehicle_estimator::Estimator estimator_;
    std::uint64_t malformed_ = 0;
};

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_PIPELINE_H
