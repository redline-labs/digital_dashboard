// SPDX-License-Identifier: GPL-3.0-or-later

#include "pipeline.h"

#include <algorithm>

namespace state_estimator
{

Pipeline::Pipeline(const NodeConfig& config) : gnss_(config.assembler), estimator_(estimatorConfig(config)) {}

Fed Pipeline::onMessage(std::string_view schema, std::span<const std::uint8_t> payload, double arrival)
{
    const Fed fed = feed(schema, payload, arrival, gnss_, imu_);
    if (fed == Fed::malformed) ++malformed_;
    return fed;
}

std::vector<vehicle_estimator::VehicleState> Pipeline::advance(double now)
{
    auto epochs = gnss_.poll(now);
    auto samples = imu_.take();

    // In arrival order, as the estimator would have seen them live: its clock
    // alignment is built on arrival times, and its GNSS queue waits for the
    // IMU to cover each epoch.
    std::vector<vehicle_estimator::VehicleState> out;
    std::size_t e = 0;
    for (const auto& s : samples)
    {
        while (e < epochs.size() && epochs[e].host_time <= s.host_time) estimator_.addGnss(epochs[e++]);
        estimator_.addImu(s);
        estimator_.process();
        if (const auto state = estimator_.latest()) out.push_back(*state);
    }
    for (; e < epochs.size(); ++e) estimator_.addGnss(epochs[e]);
    estimator_.process();
    return out;
}

}  // namespace state_estimator
