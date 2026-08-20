// SPDX-License-Identifier: GPL-3.0-or-later
//
// A registry holding every CAN backend this build has.
//
// A separate target from `can` because `can` is what the backends are written
// against: putting this function there would make a library that hands out a
// loopback channel depend on libusb. It is small enough that the split costs
// nothing and keeps the dependency arrows pointing one way.
#ifndef CAN_BACKENDS_REGISTRY_H
#define CAN_BACKENDS_REGISTRY_H

#include "can/backend.h"
#include "can_motec/utc_backend.h"
#include "can_pcan/pcan_backend.h"
#include "can_trc/trc_backend.h"

namespace can
{

struct DefaultRegistryOptions
{
    pcan::PcanOptions pcan;
    motec::MotecOptions motec;
    trc::ReplayOptions trc;
    // Leave a backend out entirely. Useful for a test that wants deterministic
    // behaviour on a machine that happens to have a dongle plugged in.
    bool includePcan { true };
    bool includeMotec { true };
    bool includeSocketCan { true };
    bool includeVirtual { true };
    bool includeTrc { true };
};

Registry make_default_registry(const DefaultRegistryOptions& options = {});

} // namespace can

#endif // CAN_BACKENDS_REGISTRY_H
