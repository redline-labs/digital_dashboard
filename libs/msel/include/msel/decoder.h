// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns CAN frames from an MSEL Master Relay into decoded signals.
//
// This is the stateful half of the library, and it is deliberately the only
// part that knows the relay has been re-addressed. The base CAN address is
// user-configurable, so the three periodic messages do not sit at fixed
// identifiers -- but the DBC, like every DBC, describes them at one fixed set.
//
// The mapping between the two happens here, as an explicit comparison against
// the three identifiers this relay is configured to use. It is written that way
// rather than as `id - offset` handed to the generated parser, because
// subtracting an offset from every frame on the bus makes unrelated traffic
// alias onto the relay's messages: with a base of 0x6F4, an arbitrary 0x704
// would decode as a status frame and publish invented voltages. Matching first
// and translating second cannot do that.
//
// Nothing here transmits, and nothing here knows about zenoh. Feeding it frames
// and publishing what comes out is the node's job.
#ifndef MSEL_DECODER_H
#define MSEL_DECODER_H

#include <functional>
#include <optional>

#include "helpers/can_frame.h"
#include "msel/protocol.h"

namespace msel
{

class Decoder
{
  public:
    // What a frame turned out to be. `No` covers everything that is not this
    // relay's, which on a shared bus is nearly all of it.
    enum class Accepted
    {
        No,
        Status,
        Info,
        SwitchState,
        ConfigResponse,
    };

    // The most recent of each message. `info` is what backs a settings
    // read-back: it carries the relay's stored configuration, so a caller can
    // be told the current settings without asking the device anything.
    //
    // All four start empty. A relay that has just been powered up, or one whose
    // firmware predates the switch-state message, will leave some of them that
    // way indefinitely -- absence here is ordinary, not an error.
    struct Snapshot
    {
        std::optional<StatusFrame> status;
        std::optional<InfoFrame> info;
        std::optional<SwitchStateFrame> switchState;
        std::optional<ConfigResponse> lastConfigResponse;
    };

    explicit Decoder(Addresses addresses = {});

    const Addresses& addresses() const { return mAddresses; }

    // Follows the relay to a new base address, in place.
    //
    // This exists because the obvious alternative -- assigning a freshly
    // constructed Decoder over this one -- silently throws away every
    // registered callback, and the failure is invisible: frames arrive at the
    // new address, decode correctly, update the snapshot, and reach nobody.
    // Changing the address is exactly the moment that would happen, so there is
    // a way to do it that cannot.
    //
    // The snapshot is kept. It came from the same physical relay a moment ago,
    // and discarding it would make a settings read-back report "we have not
    // heard from the relay" immediately after a successful re-addressing.
    void setAddresses(Addresses addresses) { mAddresses = addresses; }

    // Decodes one frame, updates the snapshot, and fires the matching callback.
    // A frame that is not this relay's, or that is too short for the message
    // its identifier claims, returns `No` and changes nothing.
    Accepted onFrame(const helpers::CanFrame& frame);

    // One callback per message; setting a second replaces the first. Register
    // before frames start arriving -- this class is not thread safe, and is not
    // meant to be.
    void onStatus(std::function<void(const StatusFrame&)> handler);
    void onInfo(std::function<void(const InfoFrame&)> handler);
    void onSwitchState(std::function<void(const SwitchStateFrame&)> handler);
    void onConfigResponse(std::function<void(ConfigResponse)> handler);

    const Snapshot& snapshot() const { return mSnapshot; }

  private:
    Addresses mAddresses;
    Snapshot mSnapshot;

    std::function<void(const StatusFrame&)> mStatusHandler;
    std::function<void(const InfoFrame&)> mInfoHandler;
    std::function<void(const SwitchStateFrame&)> mSwitchStateHandler;
    std::function<void(ConfigResponse)> mConfigResponseHandler;
};

} // namespace msel

#endif // MSEL_DECODER_H
