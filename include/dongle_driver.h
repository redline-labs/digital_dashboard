#ifndef DONGLE_DRIVER_H_
#define DONGLE_DRIVER_H_

#include "app_config.h"
#include "message.h"

#include <atomic>
#include <stdbool.h>
#include <stdint.h>
#include <string_view>
#include <thread>

// Forward declaration from libusb to keep things tidy.
struct libusb_device_handle;

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

  Done,
  Fail,
};

class DongleDriver
{
  public:
    static constexpr uint16_t kUsbVid = 0x1314u;
    static constexpr uint16_t kUsbPid = 0x1521u;

    static constexpr uint8_t kInterfaceNumber = 0x00;
    static constexpr uint8_t kEndpointInAddress = 0x81u;
    static constexpr uint8_t kEndpointOutAddress = 0x01u;

    static constexpr uint16_t kUsbDefaultTimeoutMs = 1000u;

    static constexpr uint16_t kUsbConfigDelayMs = 25u;

    static constexpr uint16_t kHeartbeatTimeMs = 2000u;

    DongleDriver(app_config_t cfg, bool debug = false);
    ~DongleDriver();

    void step();
    void stop();

    static std::string_view libusb_version();

    void register_frame_ready_callback(std::function<void(const uint8_t* buffer, uint32_t buffer_len)> cb);

  private:
    app_config_t _app_cfg;
    libusb_device_handle* _device_handle;
    int _hotplug_callback_handle;
    DeviceStep _current_step;

    std::thread _event_thread;
    std::thread _read_thread;
    std::thread _heartbeat_thread;
    std::atomic<bool> _event_thread_should_run;
    std::atomic<bool> _read_thread_should_run;
    std::atomic<bool> _heartbeat_thread_should_run;

    std::function<void(const uint8_t* buffer, uint32_t buffer_len)> _frame_ready_callback;

    bool find_dongle();

    void read_thread();
    void libusb_event_thread();
    void heartbeat_thread();

    void decode_dongle_response(MessageHeader header, const uint8_t* buffer);
};



#endif // DONGLE_DRIVER_H_