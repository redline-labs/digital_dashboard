#ifndef DONGLE_DRIVER_H_
#define DONGLE_DRIVER_H_

#include <atomic>
#include <stdbool.h>
#include <stdint.h>
#include <string_view>
#include <thread>

// Forward declaration from libusb to keep things tidy.
struct libusb_device_handle;

class DongleDriver
{
  public:
    static constexpr uint16_t kUsbVid = 0x1314u;
    static constexpr uint16_t kUsbPid = 0x1521u;

    static constexpr uint8_t kInterfaceNumber = 0x00;
    static constexpr uint8_t kEndpointInAddress = 0x81u;
    static constexpr uint8_t kEndpointOutAddress = 0x01u;

    DongleDriver(bool debug = false);
    ~DongleDriver();

    void stop();

    bool find_dongle();

    static std::string_view libusb_version();

    void tear_down();

  private:
    libusb_device_handle* _device_handle;
    int _hotplug_callback_handle;

    std::thread _event_thread;
    std::atomic<bool> _should_run;

    void libusb_event_thread();
};



#endif // DONGLE_DRIVER_H_