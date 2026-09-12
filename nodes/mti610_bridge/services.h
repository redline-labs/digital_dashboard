// SPDX-License-Identifier: GPL-3.0-or-later
//
// The device's configuration, exposed as zenoh services.
//
// EVERY ONE OF THESE COSTS DATA. An MTi answers configuration messages only in
// Config state and emits MTData2 only in Measurement state, so anything that
// touches the configuration stops the stream for as long as it takes. That is
// a property of the device rather than of this node, and it is why
// get_output_config reports the cached answer unless asked to refresh.
//
// The callbacks run on zenoh service threads. They never touch the serial port
// themselves -- the reader thread owns it -- so they work by asking
// StreamClient for a reconfigure and waiting on its generation counter. A
// service that reached into the port would race the reader for the same bytes.

#ifndef MTI610_SERVICES_H
#define MTI610_SERVICES_H

#include <memory>
#include <string>
#include <vector>

#include "mti610/output_config.h"
#include "mti610/stream_client.h"

namespace mti610_node
{

class Services
{
  public:
    Services(mti610::StreamClient& client, const std::string& topicPrefix,
             mti610::ConfigMode mode, std::vector<mti610::OutputEntry> desired);
    ~Services();

    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
    Services(Services&&) = delete;
    Services& operator=(Services&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// Shared with main.cpp's status message, so a change reported by a service and
// one reported in the status read identically.
void fill_output_entry(auto builder, const mti610::OutputEntry& entry);
void fill_change(auto builder, const mti610::Change& change);

} // namespace mti610_node

#endif // MTI610_SERVICES_H
