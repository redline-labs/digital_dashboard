// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_RETURN_BUTTON_POLICY_H_
#define CARPLAY_RETURN_BUTTON_POLICY_H_

namespace carplay
{

// Whether the CarPlay widget draws its return button.
//
// Only while no phone session is live: once the phone is recording, its video
// fills the widget, every touch belongs to the phone, and the phone's own
// manufacturer tile is the way back. "Live" needs all three -- session state
// that is still arriving, a device, and recording -- because a driver that has
// died leaves its last "recording" message standing, and trusting that would
// strand the driver on a frozen frame with no button.
constexpr bool returnButtonVisible(bool enabled, bool session_fresh, bool device_connected, bool recording)
{
    return enabled && !(session_fresh && device_connected && recording);
}

}  // namespace carplay

#endif  // CARPLAY_RETURN_BUTTON_POLICY_H_
