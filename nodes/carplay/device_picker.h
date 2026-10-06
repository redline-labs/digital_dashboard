// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_DEVICE_PICKER_H_
#define CARPLAY_DEVICE_PICKER_H_

#include "apple_usb/usb_device.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace carplay
{

// Whether an Apple device can be a CarPlay phone at all: it has the CarPlay
// USB configuration to switch into, or is already in it. A keyboard, an audio
// adapter or a charging dock enumerates as Apple too and has one or two.
inline bool offersCarPlay(const apple_usb::DeviceInfo& device)
{
    return device.active_configuration == apple_usb::kCarPlayConfiguration ||
           device.num_configurations >= apple_usb::kCarPlayConfiguration;
}

// Which of the attached candidates to bring up next.
//
// The first device that enumerated used to win every time, so a second phone,
// or one that kept failing, stood in front of the one that would have worked
// for as long as both were plugged in. Now each port's consecutive failures are
// counted and the device with the fewest goes first, in enumeration order on a
// tie. A lone phone is still retried -- the supervisor's backoff paces that --
// and a port's count is forgotten when it unplugs or succeeds.
class DevicePicker
{
  public:
    // Candidates in the order to try them.
    std::vector<apple_usb::DeviceInfo> order(std::vector<apple_usb::DeviceInfo> devices)
    {
        std::erase_if(devices, [](const apple_usb::DeviceInfo& d) { return !offersCarPlay(d); });
        forgetAbsent(devices);
        std::stable_sort(devices.begin(), devices.end(),
                         [this](const apple_usb::DeviceInfo& a, const apple_usb::DeviceInfo& b) {
                             return failuresOf(a) < failuresOf(b);
                         });
        return devices;
    }

    void failed(const apple_usb::PortPath& port) { ++failures_[port.toString()]; }
    void succeeded(const apple_usb::PortPath& port) { failures_.erase(port.toString()); }

    int failuresOf(const apple_usb::DeviceInfo& device) const
    {
        const auto it = failures_.find(device.port.toString());
        return it == failures_.end() ? 0 : it->second;
    }

  private:
    void forgetAbsent(const std::vector<apple_usb::DeviceInfo>& present)
    {
        std::erase_if(failures_, [&present](const auto& entry) {
            return std::none_of(present.begin(), present.end(), [&entry](const auto& d) {
                return d.port.toString() == entry.first;
            });
        });
    }

    std::map<std::string, int> failures_;
};

}  // namespace carplay

#endif  // CARPLAY_DEVICE_PICKER_H_
