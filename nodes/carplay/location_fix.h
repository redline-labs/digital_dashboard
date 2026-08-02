// SPDX-License-Identifier: GPL-3.0-or-later
//
// A GPS fix, on its way to the phone.
//
// Its own header because it is plain data with two unrelated consumers: the
// zenoh bridge, which receives fixes from a GPS source, and the node config,
// which can carry a static one for bench-testing the uplink. Keeping it in
// zenoh_bridge.h meant anything that merely wanted to name a fix had to pull in
// zenoh and the generated schemas.
#ifndef CARPLAY_LOCATION_FIX_H_
#define CARPLAY_LOCATION_FIX_H_

#include <cstdint>

namespace carplay
{

struct LocationFix
{
    double latitude_deg = 0.0;
    double longitude_deg = 0.0;
    double altitude_m = 0.0;
    double speed_knots = 0.0;
    double course_deg = 0.0;
    uint32_t satellites = 0;
    double hdop = 1.0;
    uint64_t utc_epoch_ms = 0;
    bool valid = true;
};

}  // namespace carplay

#endif  // CARPLAY_LOCATION_FIX_H_
