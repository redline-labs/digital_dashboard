// SPDX-License-Identifier: GPL-3.0-or-later
//
// A CAN bus made of a file.
//
// `trc:<path>` opens a channel that hands out the frames a .trc trace recorded,
// spaced the way they were recorded. `trc:<path>/<bus>` narrows that to one bus
// of a multi-bus trace.
//
// That second form is the reason this is a backend rather than a tool. A trace
// carries a Bus column numbered 1 to 16, and can::ChannelId already spells
// "channel N of device D" as `backend:device/N` -- so one file with three buses
// in it becomes three can::Channels, which is exactly the shape the PCAN
// backend uses for a two-channel dongle. Nothing above can::Registry has to
// learn anything: a bridge asks for `trc:/logs/run.trc/2` and gets a Channel.
//
// The path lands in ChannelId::device without any escaping because
// parse_channel_id only reads a trailing `/N` as a channel number when the
// whole segment is digits. `trc:/var/log/run.trc` keeps its slashes; the one
// path this cannot express is a file whose name is a bare number.
//
// What this backend is *not* is the way to record a trace. A Channel::send()
// only ever sees frames this process transmits, and a trace worth keeping has
// both directions of a bus in it, so recording lives as a tap inside the bridge
// node instead. send() here drops and counts, the same as transmitting onto a
// bus with nothing else attached.
#ifndef CAN_TRC_TRC_BACKEND_H
#define CAN_TRC_TRC_BACKEND_H

#include "can/backend.h"

#include <memory>

namespace can::trc
{

struct ReplayOptions
{
    // Play frames at the intervals the trace recorded. Turning this off replays
    // as fast as the reader can go, which is what a test wants and what an
    // import wants; it is not what watching a dashboard wants.
    bool paced { true };

    // Multiplies the recorded rate. 2.0 is twice as fast, 0.5 is half.
    double speed { 1.0 };

    // Start again at the end rather than going quiet.
    bool loop { false };
};

// Backend-wide, matching how PcanOptions reaches the PCAN backend. A per-file
// speed would need OpenOptions to grow a field every backend then has to
// ignore, and nothing has yet wanted two traces at two rates in one process.
std::shared_ptr<Backend> make_trc_backend(const ReplayOptions& options = {});

} // namespace can::trc

#endif // CAN_TRC_TRC_BACKEND_H
