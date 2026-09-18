// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tail of every CAN decoder node's main(): megasquirt, motec_ltc, motec_m1,
// motec_pdm and racegrade_tc8 each carried the same twenty-five lines of it.
#ifndef NODE_HEALTH_CAN_DECODER_H_
#define NODE_HEALTH_CAN_DECODER_H_

#include "helpers/can_frame.h"

#include <functional>
#include <string>
#include <string_view>

namespace node_health
{

// Subscribes to `can_key`, hands every frame to `decode`, and reports health
// as `node_name` until SIGINT or SIGTERM. `decode` returns whether the frame
// was one of this node's; it runs on a zenoh thread.
//
// Two activity checks: `can_rx` (any frame within 1 s) and `decoded` (one of
// ours within 2 s), because a quiet bus and a bus carrying only other devices
// look identical otherwise.
void runCanDecoder(std::string_view node_name, const std::string& can_key,
                   std::function<bool(const helpers::CanFrame&)> decode);

}  // namespace node_health

#endif  // NODE_HEALTH_CAN_DECODER_H_
