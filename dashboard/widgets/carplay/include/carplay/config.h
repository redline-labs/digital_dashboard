#ifndef CARPLAY_CONFIG_H
#define CARPLAY_CONFIG_H

#include <string>
#include <cstdint>
#include "reflection/reflection.h"

REFLECT_ENUM(DriveType,
    LHD,
    RHD
)

REFLECT_ENUM(WiFiType,
    Disabled,
    WiFi_2_4_GHz,
    WiFi_5_GHz
)

REFLECT_ENUM(MicType,
    Box,
    OS
)

REFLECT_STRUCT(carplay_phone_config_t,
    (int32_t, frame_interval, 30)
)

// Defaults previously set here are handled elsewhere

REFLECT_STRUCT(android_auto_phone_config_t,
    (int32_t, frame_interval, 30)
)

// Defaults previously set here are handled elsewhere

REFLECT_STRUCT(phone_config_t,
    (carplay_phone_config_t, car_play, carplay_phone_config_t{}),
    (android_auto_phone_config_t, android_auto, android_auto_phone_config_t{})
)

REFLECT_STRUCT(CarplayConfig_t,
    (bool, libusb_debug, false),
    (uint16_t, width_px, 800),
    (uint16_t, height_px, 600),
    (uint8_t, fps, 30),
    (uint16_t, dpi, 100),
    (uint8_t, format, 0),
    (uint8_t, i_box_version, 0),
    (uint16_t, phone_work_mode, 0),
    (uint32_t, packet_max, 1024),
    (std::string, box_name, ""),
    (bool, night_mode, false),
    (DriveType, drive_type, DriveType::LHD),
    (uint16_t, media_delay, 0),
    (bool, audio_transfer_mode, false),
    (WiFiType, wifi_type, WiFiType::Disabled),
    (MicType, mic_type, MicType::Box),
    (phone_config_t, phone_config, phone_config_t{}),
    (uint32_t, audio_device_buffer_size, 1024)
)

#endif // CARPLAY_CONFIG_H