// SPDX-License-Identifier: GPL-3.0-or-later
//
// A simulated MSEL Master Relay, so the protocol can be exercised without one
// on the bench. Modelled on canopen::StubDevice, and there for the same reason:
// the interesting behaviour of this device is what it does to a command, and
// that is not something a decode-only test can reach.
//
// One property matters more than the rest. This class packs its periodic frames
// **by hand from the manual's byte tables**, not through the generated DBC code.
// If it encoded with the DBC and the test decoded with the DBC, the test would
// only prove the DBC is self-consistent -- it would pass just as happily if
// every signal were in the wrong place. Packing independently is what makes
// decoding those frames a real check that the DBC matches the hardware.
//
// It also models the two things that make this device awkward to configure: a
// command is ignored outright unless the external kill switch is being held,
// and most settings do not take effect until the relay is power-cycled.
#ifndef MSEL_STUB_RELAY_H
#define MSEL_STUB_RELAY_H

#include <optional>
#include <vector>

#include "helpers/can_frame.h"
#include "msel/protocol.h"

namespace msel
{

class StubRelay
{
  public:
    // Everything the relay would report. Defaults are the manual's own worked
    // example values (12.56V, 54.5A, 25.2C) on a unit new enough to transmit
    // the switch-state message.
    struct State
    {
        double voltageIn { 12.56 };
        double voltageOut { 12.56 };
        double loadCurrent { 54.5 };
        double temperatureInternal { 25.2 };
        uint8_t warnings { 0u };
        uint8_t statusRaw { static_cast<uint8_t>(Status::Normal) };
        uint16_t serialNo { 64666u };
        std::chrono::milliseconds timeSinceShutdown { 0 };
        uint8_t shutdownCauseRaw { 0u };
        uint8_t shutdownCause2Raw { 0u };
        uint8_t switchStateRaw { static_cast<uint8_t>(SwitchState::NormalCalNormalExternalSwitch) };

        // Units below serial number 64666 do not transmit the switch-state
        // message at all. Set false to model one.
        bool transmitsSwitchState { true };

        Config config {};
    };

    explicit StubRelay(Addresses addresses = {});

    State& state() { return mState; }
    const State& state() const { return mState; }
    const Addresses& addresses() const { return mAddresses; }

    // Whether a human is holding the external kill switch. False from the
    // start, which is what makes "I sent the command and nothing happened" the
    // default behaviour here just as it is on the car.
    void setExternalKillSwitchHeld(bool held) { mExternalKillHeld = held; }
    bool externalKillSwitchHeld() const { return mExternalKillHeld; }

    // One transmission cycle: two frames, or three on a unit new enough for the
    // switch-state message.
    std::vector<helpers::CanFrame> periodicFrames() const;

    // Feeds one frame to the relay. Returns the frame it would answer with, or
    // nullopt when it stays silent -- which is the case for traffic that is not
    // addressed to it, and for a command sent without the external kill switch
    // held.
    std::optional<helpers::CanFrame> onFrame(const helpers::CanFrame& frame);

    // True once a remote kill has been accepted. The relay stays isolated until
    // it is reset.
    bool isolated() const { return mIsolated; }

    // Settings that have been stored but are waiting on a power cycle. Applying
    // them is what powerCycle() does; until then the relay keeps reporting and
    // behaving as it did before.
    bool hasPendingConfig() const { return mPendingConfig.has_value(); }
    void powerCycle();

  private:
    std::optional<helpers::CanFrame> handleCommand(const helpers::CanFrame& frame);
    helpers::CanFrame makeResponse(ConfigResponse response) const;

    Addresses mAddresses;
    State mState;
    bool mExternalKillHeld { false };
    bool mIsolated { false };
    uint32_t mKillAddress { kDefaultKillAddress };
    std::optional<Config> mPendingConfig;
    std::optional<uint32_t> mPendingKillAddress;
};

} // namespace msel

#endif // MSEL_STUB_RELAY_H
