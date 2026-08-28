// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's zenoh services: read the channel, change it, read the identity.
//
// They run on zenoh threads and call straight into xpr::Radio, which
// serialises the socket with a mutex -- so a service call and the node's own
// pump can overlap safely, and a service call that has to wait for a reply
// does not stop broadcasts being collected.
//
// NOTHING HERE PUBLISHES. A channel change reaches the bus through the radio's
// own 0xB40D broadcast, which the node's loop picks up like any other, so the
// topic says what the RADIO did rather than what the service asked for. The
// two cannot drift apart, and every publisher stays on one thread.

#ifndef XPR_NODE_SERVICES_H
#define XPR_NODE_SERVICES_H

#include <memory>

#include "node_config.h"
#include "publishers.h"
#include "xpr/radio.h"

namespace xpr_node
{

class Services
{
  public:
    struct Deps
    {
        xpr::Radio* radio { nullptr };
        SharedState* state { nullptr };
        const NodeConfig* config { nullptr };
    };

    explicit Services(Deps deps);
    ~Services();

    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// Read where the radio is, and how many zones and channels it has, in one go.
// Shared between the service and the node's own post-connect read so the two
// cannot disagree about what a refresh means.
xpr::Result<ChannelState> read_channel(xpr::Radio& radio);

} // namespace xpr_node

#endif // XPR_NODE_SERVICES_H
