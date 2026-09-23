// SPDX-License-Identifier: GPL-3.0-or-later
//
// A simulated drive as the bus would carry it: the capnp messages
// mti610_bridge and bd992_bridge would publish, on their keys, in arrival
// order. The node's end-to-end test feeds these through Pipeline, and
// tools/estimator_sim writes them into a bag.
//
// Not shipped: it links the simulator.

#ifndef STATE_ESTIMATOR_SIM_BUS_H
#define STATE_ESTIMATOR_SIM_BUS_H

#include "vehicle_estimator/sim/scenario.h"

#include <cstdint>
#include <string>
#include <vector>

namespace state_estimator
{

struct BusMessage
{
    std::string key;
    std::string schema;
    std::vector<std::uint8_t> payload;
    double arrival = 0.0;  // host seconds
};

struct BusLayout
{
    std::string imuPrefix{"nodes/mti610/mtdata2"};
    std::string gnssPrefix{"nodes/bd992/gsof"};
    // Accuracy and fix type at 1 Hz, as the shipped bd992.yaml sends them.
    unsigned slowEvery{10};
};

std::vector<BusMessage> toBus(const vehicle_estimator::sim::Scenario& scenario, const BusLayout& layout = {});

// The truth at scenario time t, in the estimator's output struct, so it can
// be published on the same schema and plotted against the estimate field by
// field.
vehicle_estimator::VehicleState truthState(const vehicle_estimator::sim::Scenario& scenario, double t);

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_SIM_BUS_H
