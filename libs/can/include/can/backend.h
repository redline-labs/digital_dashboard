// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finding CAN channels and opening them.
//
// This is the layer that makes "one device, several channels" and "one device
// is a channel" look the same from above.
//
//     Registry              backends by name
//        |
//        +-- SocketCanBackend    enumerate() -> one id per kernel interface
//        |                       open()      -> one socket, done
//        |
//        +-- PcanBackend         enumerate() -> one id per (dongle, channel)
//        |                       open()      -> finds or creates the shared
//        |                                      PcanDevice for that dongle and
//        |                                      returns a channel onto it
//        |
//        +-- VirtualBackend      loopback, no hardware
//
// A PCAN-USB Pro FD is a single USB handle with a single pair of bulk
// endpoints carrying both channels' traffic interleaved. That cannot be opened
// twice, so the backend keeps one device object per dongle, reference-counted
// by the channels handed out of it, and the demultiplexing lives there. Nothing
// above this file knows or cares: it asks for `pcan:0/1` and gets a Channel.
//
// The registry is deliberately not a singleton that self-populates. A caller
// constructs one and registers the backends it wants, which is what lets a test
// build a registry containing only the virtual backend and get deterministic
// behaviour on a machine that happens to have a dongle plugged in.
#ifndef CAN_BACKEND_H
#define CAN_BACKEND_H

#include "can/channel.h"
#include "can/channel_id.h"
#include "can/error.h"

#include <memory>
#include <string>
#include <vector>

namespace can
{

// How a channel should be set up at open time. Passing this to open() rather
// than making the caller configure afterwards means a backend that can only
// set its bit rate before the interface comes up -- SocketCAN -- does not need
// a different call order from one that can change it any time.
struct OpenOptions
{
    Bitrate bitrate;
    bool listenOnly { false };
    // Bring the channel up as part of opening it.
    bool start { true };
    // How many frames the backend may buffer before it starts dropping. The
    // default holds about a second of a busy 500 kbit/s bus.
    size_t rxQueueDepth { 8192 };
};

class Backend
{
public:
    virtual ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // "socketcan", "pcan", "virtual".
    virtual const std::string& name() const = 0;

    // Everything this backend can see right now. Never fails: a backend with
    // no hardware, or one that cannot run on this platform, returns an empty
    // list. A channel that exists but cannot be opened comes back with
    // `available` false and a reason, which is more useful than omitting it --
    // "your dongle is held by the kernel driver" beats "no dongle found".
    virtual std::vector<ChannelInfo> enumerate() = 0;

    virtual Result<std::shared_ptr<Channel>> open(const ChannelId& id,
                                                  const OpenOptions& options) = 0;

protected:
    Backend() = default;
};

class Registry
{
public:
    Registry();
    ~Registry();

    // Movable but not copyable: a registry owns shared handles to backends,
    // and two registries holding the same PCAN backend would each think they
    // could hand out its channels.
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept;
    Registry& operator=(Registry&&) noexcept;

    void add(std::shared_ptr<Backend> backend);

    // Every channel every registered backend can see, in registration order.
    std::vector<ChannelInfo> enumerate() const;

    Result<std::shared_ptr<Channel>> open(const ChannelId& id, const OpenOptions& options) const;
    Result<std::shared_ptr<Channel>> open(const std::string& id, const OpenOptions& options) const;

    std::vector<std::string> backend_names() const;

private:
    std::vector<std::shared_ptr<Backend>> backends_;
};

} // namespace can

#endif // CAN_BACKEND_H
