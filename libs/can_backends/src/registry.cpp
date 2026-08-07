// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_backends/registry.h"

#include "can/virtual_backend.h"
#include "can_socketcan/socketcan_backend.h"

namespace can
{

Registry make_default_registry(const DefaultRegistryOptions& options)
{
    Registry registry;

    // Order decides only what enumerate() lists first, so real hardware comes
    // before the loopback -- a `--list` should put the thing that was plugged
    // in at the top.
    if (options.includePcan)
    {
        registry.add(pcan::make_pcan_backend(options.pcan));
    }
    if (options.includeSocketCan)
    {
        registry.add(socketcan::make_socketcan_backend());
    }
    if (options.includeVirtual)
    {
        registry.add(make_virtual_backend());
    }
    if (options.includeTrc)
    {
        registry.add(trc::make_trc_backend(options.trc));
    }

    return registry;
}

} // namespace can
