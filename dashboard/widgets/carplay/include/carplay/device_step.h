#ifndef DEVICE_STEP_H_
#define DEVICE_STEP_H_

#include <string_view>

enum class DeviceStep
{
  Init,
  SendDPI,
  SendOpen,
  SendNightMode,
  SendDriveHand,
  SendChargeMode,
  SendBoxName,
  SendBoxSettings,
  SendWiFiEnable,
  SendWiFiType,
  SendMicType,
  SendAudioTransferMode,
  SendWiFiConnect,

  Done,
  Fail,
};


constexpr std::string_view device_step_to_string(DeviceStep step)
{
    switch (step)
    {
        case (DeviceStep::Init): return "Init";
        case (DeviceStep::SendDPI): return "SendDPI";
        case (DeviceStep::SendOpen): return "SendOpen";
        case (DeviceStep::SendNightMode): return "SendNightMode";
        case (DeviceStep::SendDriveHand): return "SendDriveHand";
        case (DeviceStep::SendChargeMode): return "SendChargeMode";
        case (DeviceStep::SendBoxName): return "SendBoxName";
        case (DeviceStep::SendBoxSettings): return "SendBoxSettings";
        case (DeviceStep::SendWiFiEnable): return "SendWiFiEnable";
        case (DeviceStep::SendWiFiType): return "SendWiFiType";
        case (DeviceStep::SendMicType): return "SendMicType";
        case (DeviceStep::SendAudioTransferMode): return "SendAudioTransferMode";
        case (DeviceStep::SendWiFiConnect): return "SendWiFiConnect";
        case (DeviceStep::Done): return "Done";
        case (DeviceStep::Fail): return "Fail";
        default: return "UNKNOWN";
    }
}

#endif  // DEVICE_STEP_H_