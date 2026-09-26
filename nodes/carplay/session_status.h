// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session state the dashboard sees, and the one place it is published
// from. The bring-up moves the phase along as a phone comes up; the AirPlay
// receiver reports recording and the microphone. Both used to publish whole
// states of their own, and each needed telling when to stand down so it did
// not overwrite the other's half of the picture.
#ifndef CARPLAY_SESSION_STATUS_H_
#define CARPLAY_SESSION_STATUS_H_

#include "zenoh_bridge.h"

#include <cstdint>
#include <functional>
#include <mutex>

namespace carplay
{

const char* phaseName(SessionPhase phase);

class SessionStatus
{
  public:
    using Publish = std::function<void(const SessionState&)>;

    explicit SessionStatus(Publish publish);

    // Publishes at once when anything changed. deviceConnected follows the
    // phase -- any phase but Idle means a phone is attached -- and leaving
    // Recording drops the microphone request with it.
    void setPhase(SessionPhase phase);

    // From the receiver. Losing recording steps back to AirplayHandshake, but
    // only from Recording: the receiver reports it during teardown, after an
    // unplug may already have moved the phase to Idle.
    void setRecording(bool recording);

    void setMicrophone(bool active, uint32_t sample_rate_hz, uint8_t channels);

    // What the phone was told, so a widget can match its chrome to it.
    void setDisplay(bool night_mode, uint16_t width_px, uint16_t height_px);

    // zenoh keeps no last value; a dashboard started mid-session learns the
    // state from this.
    void republish();

    SessionState state() const;

  private:
    void changePhaseLocked(SessionPhase phase);

    mutable std::mutex mutex_;
    SessionState state_;
    // Called under mutex_, so publishes leave in the order the changes were
    // made and a heartbeat cannot resend a state that has been superseded.
    Publish publish_;
};

}  // namespace carplay

#endif  // CARPLAY_SESSION_STATUS_H_
