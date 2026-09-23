// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bus messages into the assemblers, by schema name.
//
// One entry point for every source: the live node gets (payload, schema) from
// a RawSubscriber on each bridge's prefix; --replay and the offline tool get
// the same pair from a bag. Dispatching on the schema each sample carries --
// rather than on the key -- is what lets the node subscribe with a wildcard
// and not care how the bridges name their topics.

#ifndef STATE_ESTIMATOR_DECODE_H
#define STATE_ESTIMATOR_DECODE_H

#include "gnss_assembler.h"
#include "imu_assembler.h"

#include "gsof_common.capnp.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace state_estimator
{

enum class Fed
{
    used,        // decoded and handed to an assembler
    ignored,     // a schema this node does not consume
    malformed,   // named a schema we consume but did not decode
};

Fed feed(std::string_view schema, std::span<const std::uint8_t> payload, double arrival, GnssAssembler& gnss,
         ImuAssembler& imu);

// GSOF 38's fix type, coarsened to what the estimator distinguishes.
vehicle_estimator::FixQuality fixQuality(GsofPositionFixType type);

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_DECODE_H
