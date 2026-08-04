#ifndef AGENT_CONTROL_ZENOH_METHODS_H_
#define AGENT_CONTROL_ZENOH_METHODS_H_

#include "agent_control/server.h"

namespace agent_control
{

// Registers zenoh.list, zenoh.read, zenoh.publish, zenoh.rate and
// zenoh.describe_schema.
//
// This is the highest-value debugging loop in the whole interface: publish a
// known value on a key, screenshot the gauge that subscribes to it, and see
// whether the needle moved. No test_data_publisher process, no CAN bus, no
// waiting for a car.
//
// Everything here goes through the dynamic capnp API and the generated schema
// registry, so a schema added to schemas/CMakeLists.txt works immediately with
// no change here.
void registerZenohMethods(AgentServer& server);

}  // namespace agent_control

#endif  // AGENT_CONTROL_ZENOH_METHODS_H_
