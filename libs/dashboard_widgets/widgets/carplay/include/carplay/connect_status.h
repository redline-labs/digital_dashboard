// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_CONNECT_STATUS_H_
#define CARPLAY_CONNECT_STATUS_H_

#include "carplay_session.capnp.h"

#include <string_view>

namespace carplay
{

// What the CarPlay widget says while there is no picture. Written for whoever
// is in the driver's seat, not for whoever debugs the driver: the phases are
// grouped into a few steps, and the step count is what shows the bring-up is
// moving. The one phase that can wait on the user -- pairing, behind the
// phone's Trust prompt -- is the one that says so.
struct ConnectStatus
{
    std::string_view headline;
    std::string_view hint;
    // 1..kConnectSteps while bringing a phone up, 0 when not.
    int step = 0;
};

inline constexpr int kConnectSteps = 4;

constexpr ConnectStatus connectStatus(bool session_fresh, bool device_connected, CarPlaySessionState::Phase phase)
{
    using Phase = CarPlaySessionState::Phase;
    if (!session_fresh)
    {
        return {"CarPlay unavailable", "", 0};
    }
    if (!device_connected)
    {
        return {"Connect an iPhone", "", 0};
    }
    switch (phase)
    {
        case Phase::IDLE:
            return {"Connect an iPhone", "", 0};
        case Phase::USB_CONFIG:
            return {"iPhone connected", "", 1};
        case Phase::LOCKDOWN:
            return {"Waiting for iPhone", "Unlock it, and tap Trust if asked", 2};
        case Phase::NCM_UP:
        case Phase::IAP2:
            return {"Setting up CarPlay", "", 3};
        // Recording shows here only in the moment before its first frame.
        case Phase::AIRPLAY_HANDSHAKE:
        case Phase::RECORDING:
            return {"Starting CarPlay", "", 4};
        case Phase::ERROR:
            return {"Couldn't start CarPlay", "Retrying. If it keeps failing, reconnect the iPhone", 0};
    }
    // A phase from a newer driver.
    return {"Connecting", "", 0};
}

}  // namespace carplay

#endif  // CARPLAY_CONNECT_STATUS_H_
